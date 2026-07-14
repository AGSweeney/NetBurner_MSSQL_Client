// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>

#include "gateway_config.h"

#include <cstdio>
#include <cstring>

#include <config_obj.h>
#include <config_server.h>

namespace gateway {
namespace {

GatewayConfig g_cfg{};
bool g_inited = false;

config_obj g_root{appdata, "Gateway", "Micro800 MSSQL gateway"};
config_bool g_nv_valid{g_root, false, "Valid", "Gateway config present"};
config_bool g_nv_enabled{g_root, false, "Enabled", "Gateway runtime enabled"};
config_int g_nv_revision{g_root, 0, "Revision", "Config revision"};
config_int g_nv_idempotency{g_root, 0, "Idempotency", "Idempotency mode"};
config_string g_nv_event_col{g_root, "GatewayEventId", "EventIdColumn", "Event id column"};
// Blob: compact text serialization of PLCs + mappings (tab/newline).
config_string g_nv_blob{g_root, "", "ConfigBlob", "Serialized PLC/mapping config"};

void CopyStr(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

bool IsSafeIdent(const char *s)
{
    if (!s || !s[0]) {
        return false;
    }
    for (const char *p = s; *p; ++p) {
        const char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

// SQL Server bracketed identifiers may include spaces (e.g. "DATA A").
// Reject [] so AppendIdent does not double-wrap user-supplied brackets.
bool IsSafeSqlIdent(const char *s)
{
    if (!s || !s[0]) {
        return false;
    }
    if (s[0] == ' ' || s[strlen(s) - 1] == ' ') {
        return false;
    }
    bool any = false;
    for (const char *p = s; *p; ++p) {
        const char c = *p;
        if (c == ' ') {
            continue;
        }
        any = true;
        if (c == '[' || c == ']' || c == '\t' || c == '\r' || c == '\n') {
            return false;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
              c == '#' || c == '$')) {
            return false;
        }
    }
    return any;
}

// Serialize to a simple line-oriented blob (v1).
bool EncodeBlob(const GatewayConfig &cfg, char *out, size_t cap)
{
    if (!out || cap < 8) {
        return false;
    }
    size_t n = 0;
    auto append = [&](const char *s) -> bool {
        const size_t len = strlen(s);
        if (n + len + 1 >= cap) {
            return false;
        }
        memcpy(out + n, s, len);
        n += len;
        return true;
    };

    char line[256];
    sniprintf(line, sizeof(line), "V1\t%u\t%u\t%u\n", static_cast<unsigned>(cfg.plc_count),
              static_cast<unsigned>(cfg.mapping_count), static_cast<unsigned>(cfg.config_revision));
    if (!append(line)) {
        return false;
    }

    for (uint16_t i = 0; i < cfg.plc_count; ++i) {
        const PlcConfig &p = cfg.plcs[i];
        sniprintf(line, sizeof(line), "P\t%lu\t%d\t%s\t%u.%u.%u.%u\t%lu\t%lu\t%lu\n",
                  static_cast<unsigned long>(p.id), p.enabled ? 1 : 0, p.name,
                  static_cast<unsigned>((static_cast<uint32_t>(p.ip) >> 24) & 0xFF),
                  static_cast<unsigned>((static_cast<uint32_t>(p.ip) >> 16) & 0xFF),
                  static_cast<unsigned>((static_cast<uint32_t>(p.ip) >> 8) & 0xFF),
                  static_cast<unsigned>(static_cast<uint32_t>(p.ip) & 0xFF),
                  static_cast<unsigned long>(p.poll_interval_ms),
                  static_cast<unsigned long>(p.request_timeout_ms),
                  static_cast<unsigned long>(p.retry_delay_ms));
        if (!append(line)) {
            return false;
        }
    }

    for (uint16_t i = 0; i < cfg.mapping_count; ++i) {
        const MappingConfig &m = cfg.mappings[i];
        sniprintf(line, sizeof(line), "M\t%lu\t%lu\t%d\t%s\t%s\t%s\t%d\t%s\t%s\t%s\t%u\t%d\t%s\n",
                  static_cast<unsigned long>(m.id), static_cast<unsigned long>(m.plc_id), m.enabled ? 1 : 0,
                  m.name, m.trigger_tag, m.ack_tag, static_cast<int>(m.operation), m.database, m.schema,
                  m.table, static_cast<unsigned>(m.field_count), m.include_plc_name ? 1 : 0,
                  m.plc_name_column);
        if (!append(line)) {
            return false;
        }
        for (uint16_t f = 0; f < m.field_count; ++f) {
            const FieldMapping &fld = m.fields[f];
            sniprintf(line, sizeof(line), "F\t%s\t%d\t%s\t%d\t%d\t%d\t%d\n", fld.plc_tag,
                      static_cast<int>(fld.plc_type), fld.sql_column, static_cast<int>(fld.sql_type),
                      fld.required ? 1 : 0, fld.nullable ? 1 : 0, fld.is_where_key ? 1 : 0);
            if (!append(line)) {
                return false;
            }
        }
    }
    out[n] = '\0';
    return true;
}

// Drop later rows that reuse an earlier SQL column (corrupted sticky/browse saves).
void DedupeMappingSqlColumns(MappingConfig &m)
{
    uint16_t kept = 0;
    for (uint16_t i = 0; i < m.field_count && i < kMaxFieldsPerMapping; ++i) {
        bool dup = false;
        for (uint16_t j = 0; j < kept; ++j) {
            if (strcmp(m.fields[j].sql_column, m.fields[i].sql_column) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        if (kept != i) {
            m.fields[kept] = m.fields[i];
        }
        ++kept;
    }
    m.field_count = kept;
}

bool DecodeBlob(const char *blob, GatewayConfig &cfg)
{
    ConfigSetDefaults(cfg);
    if (!blob || !blob[0]) {
        return true;
    }

    MappingConfig *curMap = nullptr;
    const char *p = blob;
    while (*p) {
        const char *eol = strchr(p, '\n');
        char line[320];
        size_t len = eol ? static_cast<size_t>(eol - p) : strlen(p);
        if (len >= sizeof(line)) {
            len = sizeof(line) - 1;
        }
        memcpy(line, p, len);
        line[len] = '\0';
        p = eol ? eol + 1 : p + len;

        if (line[0] == 'V') {
            unsigned pc = 0, mc = 0, rev = 0;
            sscanf(line, "V1\t%u\t%u\t%u", &pc, &mc, &rev);
            cfg.config_revision = rev;
            (void)pc;
            (void)mc;
        } else if (line[0] == 'P' && cfg.plc_count < kMaxPlcs) {
            PlcConfig &pl = cfg.plcs[cfg.plc_count];
            unsigned id = 0, en = 0, a = 0, b = 0, c = 0, d = 0, poll = 200, timeout = 3000, retry = 500;
            char name[kMaxNameLen]{};
            if (sscanf(line, "P\t%u\t%u\t%31[^\t]\t%u.%u.%u.%u\t%u\t%u\t%u", &id, &en, name, &a, &b, &c, &d,
                       &poll, &timeout, &retry) >= 7) {
                pl.id = id;
                pl.enabled = en != 0;
                CopyStr(pl.name, sizeof(pl.name), name);
                pl.ip = IPV4FromConst((a << 24) | (b << 16) | (c << 8) | d);
                pl.poll_interval_ms = poll ? poll : 200;
                pl.request_timeout_ms = timeout ? timeout : 3000;
                pl.retry_delay_ms = retry ? retry : 500;
                ++cfg.plc_count;
            }
        } else if (line[0] == 'M' && cfg.mapping_count < kMaxMappings) {
            MappingConfig &m = cfg.mappings[cfg.mapping_count];
            unsigned id = 0, plc = 0, en = 0, op = 0, fc = 0, incPn = 0;
            char name[kMaxNameLen]{}, trig[kMaxTagLen]{}, ack[kMaxTagLen]{};
            char db[SQL_CFG_DATABASE_LEN]{}, sch[32]{}, tbl[SQL_CFG_TABLE_LEN]{};
            char pnCol[kMaxSqlIdentLen]{};
            const int parsed = sscanf(
                line,
                "M\t%u\t%u\t%u\t%31[^\t]\t%63[^\t]\t%63[^\t]\t%u\t%63[^\t]\t%31[^\t]\t%127[^\t]\t%u\t%u\t%63[^\t]",
                &id, &plc, &en, name, trig, ack, &op, db, sch, tbl, &fc, &incPn, pnCol);
            if (parsed >= 10) {
                m.id = id;
                m.plc_id = plc;
                m.enabled = en != 0;
                CopyStr(m.name, sizeof(m.name), name);
                CopyStr(m.trigger_tag, sizeof(m.trigger_tag), trig);
                CopyStr(m.ack_tag, sizeof(m.ack_tag), ack);
                m.operation = (op == 1) ? SqlOperation::Update : SqlOperation::Insert;
                CopyStr(m.database, sizeof(m.database), db);
                CopyStr(m.schema, sizeof(m.schema), sch);
                CopyStr(m.table, sizeof(m.table), tbl);
                m.field_count = 0;
                m.include_plc_name = (parsed >= 12) && (incPn != 0);
                if (parsed >= 13) {
                    CopyStr(m.plc_name_column, sizeof(m.plc_name_column), pnCol);
                } else {
                    m.plc_name_column[0] = '\0';
                }
                curMap = &m;
                ++cfg.mapping_count;
                (void)fc;
            }
        } else if (line[0] == 'F' && curMap && curMap->field_count < kMaxFieldsPerMapping) {
            FieldMapping &f = curMap->fields[curMap->field_count];
            unsigned pt = 0, st = 0, req = 1, nul = 0, key = 0;
            char tag[kMaxTagLen]{}, col[kMaxSqlIdentLen]{};
            if (sscanf(line, "F\t%63[^\t]\t%u\t%63[^\t]\t%u\t%u\t%u\t%u", tag, &pt, col, &st, &req, &nul, &key) >= 4) {
                CopyStr(f.plc_tag, sizeof(f.plc_tag), tag);
                f.plc_type = static_cast<PlcDataType>(pt);
                CopyStr(f.sql_column, sizeof(f.sql_column), col);
                f.sql_type = static_cast<SqlDataType>(st);
                f.required = req != 0;
                f.nullable = nul != 0;
                f.is_where_key = key != 0;
                ++curMap->field_count;
            }
        }
    }
    for (uint16_t i = 0; i < cfg.mapping_count; ++i) {
        DedupeMappingSqlColumns(cfg.mappings[i]);
    }
    return true;
}

} // namespace

void ConfigSetDefaults(GatewayConfig &cfg)
{
    cfg = GatewayConfig{};
    cfg.enabled = false;
    cfg.idempotency = IdempotencyMode::None;
    CopyStr(cfg.event_id_column, sizeof(cfg.event_id_column), "GatewayEventId");
}

void ConfigInit()
{
    if (g_inited) {
        return;
    }
    g_root.ClrBranchFlag(fConfigHidden, false);
    ConfigSetDefaults(g_cfg);
    ConfigLoad();
    g_inited = true;
}

GatewayConfig &ConfigMutable()
{
    return g_cfg;
}

const GatewayConfig &ConfigGet()
{
    return g_cfg;
}

bool ConfigValidate(const GatewayConfig &cfg, char *err, size_t errCap)
{
    auto setErr = [&](const char *m) {
        if (err && errCap) {
            strncpy(err, m, errCap - 1);
            err[errCap - 1] = '\0';
        }
    };

    if (cfg.plc_count > kMaxPlcs || cfg.mapping_count > kMaxMappings) {
        setErr("Too many PLCs or mappings");
        return false;
    }

    for (uint16_t i = 0; i < cfg.mapping_count; ++i) {
        const MappingConfig &m = cfg.mappings[i];
        if (!m.trigger_tag[0] || !m.ack_tag[0]) {
            setErr("Trigger and ACK tags are required");
            return false;
        }
        if (strcmp(m.trigger_tag, m.ack_tag) == 0) {
            setErr("Trigger and ACK must be different tags");
            return false;
        }
        if (!m.table[0] || !IsSafeSqlIdent(m.table)) {
            setErr("Invalid table identifier");
            return false;
        }
        if (m.schema[0] && !IsSafeIdent(m.schema)) {
            setErr("Invalid schema identifier");
            return false;
        }
        if (m.database[0] && !IsSafeSqlIdent(m.database)) {
            setErr("Invalid database identifier");
            return false;
        }
        if (m.field_count == 0 || m.field_count > kMaxFieldsPerMapping) {
            setErr("Mapping needs 1..max fields");
            return false;
        }
        if (m.include_plc_name) {
            if (!m.plc_name_column[0] || !IsSafeSqlIdent(m.plc_name_column)) {
                setErr("PLC name column is required when Include PLC name is enabled");
                return false;
            }
            if (m.field_count >= kMaxFieldsPerMapping) {
                setErr("No room for PLC name column — reduce field count");
                return false;
            }
        }
        bool hasKey = false;
        for (uint16_t f = 0; f < m.field_count; ++f) {
            if (!m.fields[f].plc_tag[0] || !m.fields[f].sql_column[0] ||
                !IsSafeSqlIdent(m.fields[f].sql_column)) {
                setErr("Invalid field mapping (SQL column names may include spaces, but not [brackets])");
                return false;
            }
            if (m.include_plc_name && m.plc_name_column[0] &&
                strcmp(m.fields[f].sql_column, m.plc_name_column) == 0) {
                setErr("PLC name column collides with a mapped SQL column");
                return false;
            }
            for (uint16_t earlier = 0; earlier < f; ++earlier) {
                if (strcmp(m.fields[earlier].sql_column, m.fields[f].sql_column) == 0) {
                    setErr("Duplicate SQL column in mapping fields");
                    return false;
                }
            }
            if (m.fields[f].is_where_key) {
                hasKey = true;
            }
        }
        if (m.operation == SqlOperation::Update && !hasKey) {
            setErr("UPDATE requires at least one WHERE key field");
            return false;
        }
        bool plcOk = false;
        for (uint16_t p = 0; p < cfg.plc_count; ++p) {
            if (cfg.plcs[p].id == m.plc_id) {
                plcOk = true;
                break;
            }
        }
        if (!plcOk) {
            setErr("Mapping references unknown PLC id");
            return false;
        }
    }
    if (err && errCap) {
        err[0] = '\0';
    }
    return true;
}

bool ConfigSave(const GatewayConfig &cfg, char *err, size_t errCap)
{
    if (!ConfigValidate(cfg, err, errCap)) {
        return false;
    }
    static char s_blob[4096];
    s_blob[0] = '\0';
    if (!EncodeBlob(cfg, s_blob, sizeof(s_blob))) {
        if (err && errCap) {
            strncpy(err, "Config blob too large", errCap - 1);
            err[errCap - 1] = '\0';
        }
        return false;
    }
    g_cfg = cfg;
    g_nv_enabled = cfg.enabled;
    g_nv_revision = static_cast<int>(cfg.config_revision);
    g_nv_idempotency = static_cast<int>(cfg.idempotency);
    g_nv_event_col = cfg.event_id_column;
    g_nv_blob = s_blob;
    g_nv_valid = true;
    SaveConfigToStorage();
    return true;
}

bool ConfigLoad()
{
    ConfigSetDefaults(g_cfg);
    if (!g_nv_valid && g_nv_blob.length() == 0) {
        return false;
    }
    g_cfg.enabled = static_cast<bool>(g_nv_enabled);
    g_cfg.config_revision = static_cast<uint32_t>(static_cast<int>(g_nv_revision));
    g_cfg.idempotency = static_cast<IdempotencyMode>(static_cast<int>(g_nv_idempotency));
    CopyStr(g_cfg.event_id_column, sizeof(g_cfg.event_id_column), g_nv_event_col.c_str());
    DecodeBlob(g_nv_blob.c_str(), g_cfg);
    g_cfg.enabled = static_cast<bool>(g_nv_enabled);
    return true;
}

bool ConfigFindPlc(uint32_t plcId, PlcConfig &out)
{
    for (uint16_t i = 0; i < g_cfg.plc_count; ++i) {
        if (g_cfg.plcs[i].id == plcId) {
            out = g_cfg.plcs[i];
            return true;
        }
    }
    return false;
}

bool ConfigFindMapping(uint32_t mappingId, MappingConfig &out)
{
    for (uint16_t i = 0; i < g_cfg.mapping_count; ++i) {
        if (g_cfg.mappings[i].id == mappingId) {
            out = g_cfg.mappings[i];
            return true;
        }
    }
    return false;
}

} // namespace gateway
