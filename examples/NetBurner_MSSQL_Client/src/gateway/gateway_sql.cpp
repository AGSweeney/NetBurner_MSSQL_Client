// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>

#include "gateway_sql.h"
#include "gateway_config.h"
#include "gateway_queue.h"
#include "gateway_runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "sql_runtime.h"

namespace gateway {
namespace {

bool Append(char *dst, size_t cap, size_t &n, const char *s)
{
    const size_t len = strlen(s);
    if (n + len + 1 > cap) {
        return false;
    }
    memcpy(dst + n, s, len);
    n += len;
    dst[n] = '\0';
    return true;
}

bool AppendIdent(char *dst, size_t cap, size_t &n, const char *ident)
{
    if (!Append(dst, cap, n, "[") || !Append(dst, cap, n, ident) || !Append(dst, cap, n, "]")) {
        return false;
    }
    return true;
}

bool AppendSqlStringLiteral(char *dst, size_t cap, size_t &n, const char *text)
{
    // CIP STRING values are often space-padded; trim so varchar(n) columns do not truncate.
    const char *start = text ? text : "";
    while (*start == ' ' || *start == '\t') {
        ++start;
    }
    const char *end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
        --end;
    }

    if (!Append(dst, cap, n, "N'")) {
        return false;
    }
    for (const char *p = start; p < end; ++p) {
        if (*p == '\'') {
            if (!Append(dst, cap, n, "''")) {
                return false;
            }
        } else {
            char one[2] = {*p, 0};
            if (!Append(dst, cap, n, one)) {
                return false;
            }
        }
    }
    return Append(dst, cap, n, "'");
}

bool ValueAsBool(const FieldValue &v)
{
    if (v.plc_type == PlcDataType::Real) {
        return v.f32 != 0.f;
    }
    if (v.plc_type == PlcDataType::String) {
        if (!v.text[0] || strcmp(v.text, "0") == 0) {
            return false;
        }
        if (v.text[0] == 'f' || v.text[0] == 'F') {
            return false;
        }
        return true;
    }
    return v.i64 != 0;
}

int64_t ValueAsInt64(const FieldValue &v)
{
    if (v.plc_type == PlcDataType::Real) {
        return static_cast<int64_t>(v.f32);
    }
    if (v.plc_type == PlcDataType::String) {
        return static_cast<int64_t>(strtoll(v.text, nullptr, 10));
    }
    return v.i64;
}

float ValueAsFloat(const FieldValue &v)
{
    if (v.plc_type == PlcDataType::Real) {
        return v.f32;
    }
    if (v.plc_type == PlcDataType::String) {
        return static_cast<float>(strtod(v.text, nullptr));
    }
    if (v.plc_type == PlcDataType::Bool) {
        return v.i64 ? 1.f : 0.f;
    }
    return static_cast<float>(v.i64);
}

const char *ValueAsText(const FieldValue &v, char *scratch, size_t scratchCap)
{
    if (v.text[0]) {
        return v.text;
    }
    if (!scratch || scratchCap < 2) {
        return "";
    }
    if (v.plc_type == PlcDataType::Real || v.plc_type == PlcDataType::Lreal) {
        sniprintf(scratch, scratchCap, "%f", static_cast<double>(v.f32));
    } else if (v.plc_type == PlcDataType::Bool) {
        sniprintf(scratch, scratchCap, "%s", v.i64 ? "1" : "0");
    } else if (v.plc_type == PlcDataType::Usint || v.plc_type == PlcDataType::Uint ||
               v.plc_type == PlcDataType::Udint || v.plc_type == PlcDataType::Ulint) {
        sniprintf(scratch, scratchCap, "%llu", static_cast<unsigned long long>(v.i64));
    } else {
        sniprintf(scratch, scratchCap, "%lld", static_cast<long long>(v.i64));
    }
    return scratch;
}

bool AppendValue(char *dst, size_t cap, size_t &n, const FieldMapping &f, const FieldValue &v)
{
    if (!v.valid) {
        if (f.nullable || !f.required) {
            return Append(dst, cap, n, "NULL");
        }
        return false;
    }
    if (v.is_null) {
        return Append(dst, cap, n, "NULL");
    }

    // Format for the destination SQL type, converting from the captured PLC value.
    switch (f.sql_type) {
    case SqlDataType::Bit:
        return Append(dst, cap, n, ValueAsBool(v) ? "1" : "0");
    case SqlDataType::TinyInt:
    case SqlDataType::SmallInt:
    case SqlDataType::Int:
    case SqlDataType::BigInt: {
        char buf[32];
        if (v.plc_type == PlcDataType::Usint || v.plc_type == PlcDataType::Uint ||
            v.plc_type == PlcDataType::Udint || v.plc_type == PlcDataType::Ulint) {
            sniprintf(buf, sizeof(buf), "%llu",
                      static_cast<unsigned long long>(ValueAsInt64(v)));
        } else {
            sniprintf(buf, sizeof(buf), "%lld", static_cast<long long>(ValueAsInt64(v)));
        }
        return Append(dst, cap, n, buf);
    }
    case SqlDataType::Real:
    case SqlDataType::Float: {
        char buf[48];
        sniprintf(buf, sizeof(buf), "%f", static_cast<double>(ValueAsFloat(v)));
        return Append(dst, cap, n, buf);
    }
    case SqlDataType::NVarChar:
    case SqlDataType::VarChar:
    default: {
        char scratch[kMaxStringValueLen]{};
        return AppendSqlStringLiteral(dst, cap, n, ValueAsText(v, scratch, sizeof(scratch)));
    }
    }
}

} // namespace

bool ColumnUsedEarlier(const CapturedEvent &ev, uint16_t idx)
{
    for (uint16_t j = 0; j < idx; ++j) {
        if (strcmp(ev.fields[j].sql_column, ev.fields[idx].sql_column) == 0) {
            return true;
        }
    }
    return false;
}

bool BuildSqlFromEvent(const CapturedEvent &ev, char *sqlOut, size_t sqlCap, char *summaryOut, size_t summaryCap,
                       char *err, size_t errCap)
{
    auto setErr = [&](const char *m) {
        if (err && errCap) {
            strncpy(err, m, errCap - 1);
            err[errCap - 1] = '\0';
        }
    };

    if (!sqlOut || sqlCap < 16 || !ev.table[0] || ev.field_count == 0) {
        setErr("Invalid event for SQL build");
        return false;
    }

    const char *db = ev.database[0] ? ev.database : SqlRuntimeGetConfig().database;
    const char *schema = ev.schema[0] ? ev.schema : "dbo";
    size_t n = 0;
    sqlOut[0] = '\0';

    if (ev.operation == SqlOperation::Insert) {
        if (!Append(sqlOut, sqlCap, n, "INSERT INTO ")) {
            setErr("SQL buffer overflow");
            return false;
        }
        if (db && db[0]) {
            if (!AppendIdent(sqlOut, sqlCap, n, db) || !Append(sqlOut, sqlCap, n, ".")) {
                setErr("SQL buffer overflow");
                return false;
            }
        }
        if (!AppendIdent(sqlOut, sqlCap, n, schema) || !Append(sqlOut, sqlCap, n, ".") ||
            !AppendIdent(sqlOut, sqlCap, n, ev.table) || !Append(sqlOut, sqlCap, n, " (")) {
            setErr("SQL buffer overflow");
            return false;
        }

        const GatewayConfig &cfg = ConfigGet();
        const bool addEventId = (cfg.idempotency == IdempotencyMode::DestinationEventIdColumn);
        if (addEventId) {
            if (!AppendIdent(sqlOut, sqlCap, n, cfg.event_id_column) || !Append(sqlOut, sqlCap, n, ", ")) {
                setErr("SQL buffer overflow");
                return false;
            }
        }

        bool firstCol = true;
        for (uint16_t i = 0; i < ev.field_count; ++i) {
            if (ColumnUsedEarlier(ev, i)) {
                continue;
            }
            if (!firstCol && !Append(sqlOut, sqlCap, n, ", ")) {
                setErr("SQL buffer overflow");
                return false;
            }
            firstCol = false;
            if (!AppendIdent(sqlOut, sqlCap, n, ev.fields[i].sql_column)) {
                setErr("SQL buffer overflow");
                return false;
            }
        }
        if (firstCol) {
            setErr("No columns for INSERT");
            return false;
        }
        if (!Append(sqlOut, sqlCap, n, ") VALUES (")) {
            setErr("SQL buffer overflow");
            return false;
        }
        if (addEventId) {
            char idBuf[48];
            sniprintf(idBuf, sizeof(idBuf), "%llu", static_cast<unsigned long long>(ev.event_id));
            if (!Append(sqlOut, sqlCap, n, idBuf) || !Append(sqlOut, sqlCap, n, ", ")) {
                setErr("SQL buffer overflow");
                return false;
            }
        }
        bool firstVal = true;
        for (uint16_t i = 0; i < ev.field_count; ++i) {
            if (ColumnUsedEarlier(ev, i)) {
                continue;
            }
            if (!firstVal && !Append(sqlOut, sqlCap, n, ", ")) {
                setErr("SQL buffer overflow");
                return false;
            }
            firstVal = false;
            if (!AppendValue(sqlOut, sqlCap, n, ev.fields[i], ev.values[i])) {
                setErr("Failed to encode field value");
                return false;
            }
        }
        if (!Append(sqlOut, sqlCap, n, ");")) {
            setErr("SQL buffer overflow");
            return false;
        }
    } else {
        // UPDATE ... SET ... WHERE keys
        if (!Append(sqlOut, sqlCap, n, "UPDATE ")) {
            setErr("SQL buffer overflow");
            return false;
        }
        if (db && db[0]) {
            if (!AppendIdent(sqlOut, sqlCap, n, db) || !Append(sqlOut, sqlCap, n, ".")) {
                setErr("SQL buffer overflow");
                return false;
            }
        }
        if (!AppendIdent(sqlOut, sqlCap, n, schema) || !Append(sqlOut, sqlCap, n, ".") ||
            !AppendIdent(sqlOut, sqlCap, n, ev.table) || !Append(sqlOut, sqlCap, n, " SET ")) {
            setErr("SQL buffer overflow");
            return false;
        }
        bool firstSet = true;
        for (uint16_t i = 0; i < ev.field_count; ++i) {
            if (ev.fields[i].is_where_key || ColumnUsedEarlier(ev, i)) {
                continue;
            }
            if (!firstSet && !Append(sqlOut, sqlCap, n, ", ")) {
                setErr("SQL buffer overflow");
                return false;
            }
            firstSet = false;
            if (!AppendIdent(sqlOut, sqlCap, n, ev.fields[i].sql_column) || !Append(sqlOut, sqlCap, n, "=") ||
                !AppendValue(sqlOut, sqlCap, n, ev.fields[i], ev.values[i])) {
                setErr("Failed to encode SET clause");
                return false;
            }
        }
        if (!Append(sqlOut, sqlCap, n, " WHERE ")) {
            setErr("SQL buffer overflow");
            return false;
        }
        bool firstWhere = true;
        for (uint16_t i = 0; i < ev.field_count; ++i) {
            if (!ev.fields[i].is_where_key || ColumnUsedEarlier(ev, i)) {
                continue;
            }
            if (!firstWhere && !Append(sqlOut, sqlCap, n, " AND ")) {
                setErr("SQL buffer overflow");
                return false;
            }
            firstWhere = false;
            if (!AppendIdent(sqlOut, sqlCap, n, ev.fields[i].sql_column) || !Append(sqlOut, sqlCap, n, "=") ||
                !AppendValue(sqlOut, sqlCap, n, ev.fields[i], ev.values[i])) {
                setErr("Failed to encode WHERE clause");
                return false;
            }
        }
        if (firstWhere) {
            setErr("UPDATE missing WHERE keys");
            return false;
        }
        if (!Append(sqlOut, sqlCap, n, ";")) {
            setErr("SQL buffer overflow");
            return false;
        }
    }

    if (summaryOut && summaryCap) {
        sniprintf(summaryOut, summaryCap, "Gateway event %llu mapping %lu -> %s",
                  static_cast<unsigned long long>(ev.event_id), static_cast<unsigned long>(ev.mapping_id),
                  ev.table);
    }
    if (err && errCap) {
        err[0] = '\0';
    }
    return true;
}

bool BuildSqlPreviewFromMapping(const MappingConfig &map, char *sqlOut, size_t sqlCap, char *err, size_t errCap)
{
    static CapturedEvent s_ev;
    s_ev = CapturedEvent{};
    s_ev.event_id = 0;
    s_ev.plc_id = map.plc_id;
    s_ev.mapping_id = map.id;
    s_ev.operation = map.operation;
    strncpy(s_ev.database, map.database, sizeof(s_ev.database) - 1);
    strncpy(s_ev.schema, map.schema[0] ? map.schema : "dbo", sizeof(s_ev.schema) - 1);
    strncpy(s_ev.table, map.table, sizeof(s_ev.table) - 1);
    s_ev.field_count = 0;
    for (uint16_t i = 0; i < map.field_count && i < kMaxFieldsPerMapping; ++i) {
        const FieldMapping &src = map.fields[i];
        if (!src.sql_column[0]) {
            continue;
        }
        bool dupCol = false;
        for (uint16_t j = 0; j < s_ev.field_count; ++j) {
            if (strcmp(s_ev.fields[j].sql_column, src.sql_column) == 0) {
                dupCol = true;
                break;
            }
        }
        if (dupCol) {
            continue;
        }
        const uint16_t dst = s_ev.field_count;
        s_ev.fields[dst] = src;
        s_ev.values[dst].valid = true;
        s_ev.values[dst].plc_type = src.plc_type;
        switch (src.sql_type) {
        case SqlDataType::Bit:
            s_ev.values[dst].i64 = 1;
            break;
        case SqlDataType::TinyInt:
        case SqlDataType::SmallInt:
        case SqlDataType::Int:
            s_ev.values[dst].i64 = 0;
            break;
        case SqlDataType::Real:
        case SqlDataType::Float:
            s_ev.values[dst].f32 = 0.f;
            break;
        default:
            // Type-safe sample for string columns — never use <tag> placeholders
            // (those look like real SQL and fail if the column is numeric).
            strncpy(s_ev.values[dst].text, "0", sizeof(s_ev.values[dst].text) - 1);
            break;
        }
        ++s_ev.field_count;
    }
    if (map.include_plc_name && map.plc_name_column[0] && s_ev.field_count < kMaxFieldsPerMapping) {
        bool plcNameDup = false;
        for (uint16_t j = 0; j < s_ev.field_count; ++j) {
            if (strcmp(s_ev.fields[j].sql_column, map.plc_name_column) == 0) {
                plcNameDup = true;
                break;
            }
        }
        if (!plcNameDup) {
            FieldMapping &f = s_ev.fields[s_ev.field_count];
            FieldValue &v = s_ev.values[s_ev.field_count];
            f = FieldMapping{};
            strncpy(f.sql_column, map.plc_name_column, sizeof(f.sql_column) - 1);
            f.sql_type = SqlDataType::NVarChar;
            f.plc_type = PlcDataType::String;
            v = FieldValue{};
            v.valid = true;
            v.plc_type = PlcDataType::String;
            PlcConfig plc{};
            if (ConfigFindPlc(map.plc_id, plc) && plc.name[0]) {
                strncpy(v.text, plc.name, sizeof(v.text) - 1);
            } else {
                strncpy(v.text, "<PLC name>", sizeof(v.text) - 1);
            }
            ++s_ev.field_count;
        }
    }
    char summary[64]{};
    return BuildSqlFromEvent(s_ev, sqlOut, sqlCap, summary, sizeof(summary), err, errCap);
}

void SqlRequestProcessBatch()
{
    // sql_runtime polls Queue via SqlDrainOnePending when idle.
}

bool SqlDrainOnePending(char *err, size_t errCap)
{
    // Only stage when SQL is idle and no gateway mutation is already in flight.
    if (SqlRuntimeIsBusy() || HasInflightEvent()) {
        return false;
    }

    static CapturedEvent s_ev;
    uint32_t slot = 0;
    if (!QueuePeekPending(s_ev, slot)) {
        return false;
    }

    char sql[SQL_CFG_QUERY_LEN]{};
    char summary[SQL_MUTATION_SUMMARY_LEN]{};
    if (!BuildSqlFromEvent(s_ev, sql, sizeof(sql), summary, sizeof(summary), err, errCap)) {
        QueueQuarantine(s_ev.event_id, err && err[0] ? err : "SQL build failed");
        return false;
    }

    const sql_mutation_kind kind =
        (s_ev.operation == SqlOperation::Update) ? SQL_MUTATION_UPDATE : SQL_MUTATION_INSERT;

    if (!SqlRuntimeStageMutation(kind, sql, summary)) {
        if (err && errCap) {
            strncpy(err, "Failed to stage gateway mutation", errCap - 1);
            err[errCap - 1] = '\0';
        }
        return false;
    }
    SetInflightEventId(s_ev.event_id);
    SqlRuntimeRequestExecutePendingMutation();
    (void)slot;
    return true;
}

} // namespace gateway
