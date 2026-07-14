// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>

#include "gateway_serializer.h"

#include <cstring>

namespace gateway {
namespace {

void W32(uint8_t *p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

void W64(uint8_t *p, uint64_t v)
{
    W32(p, static_cast<uint32_t>(v & 0xFFFFFFFFu));
    W32(p + 4, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFu));
}

uint32_t R32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t R64(const uint8_t *p)
{
    return static_cast<uint64_t>(R32(p)) | (static_cast<uint64_t>(R32(p + 4)) << 32);
}

} // namespace

uint32_t Crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            const uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

bool SerializeEvent(const CapturedEvent &ev, uint8_t *out, size_t outCap, size_t &outLen, uint32_t &crc)
{
    outLen = 0;
    crc = 0;
    if (!out || outCap < 64) {
        return false;
    }

    // Header: magic, ver, event_id, plc, mapping, rev, secs, op, field_count
    size_t n = 0;
    auto need = [&](size_t add) -> bool {
        if (n + add > outCap) {
            return false;
        }
        return true;
    };

    if (!need(4 + 2 + 8 + 4 + 4 + 4 + 4 + 1 + 2)) {
        return false;
    }
    W32(out + n, 0x47574E54u); // "GWNT"
    n += 4;
    out[n++] = 1; // version
    out[n++] = 0;
    W64(out + n, ev.event_id);
    n += 8;
    W32(out + n, ev.plc_id);
    n += 4;
    W32(out + n, ev.mapping_id);
    n += 4;
    W32(out + n, ev.config_revision);
    n += 4;
    W32(out + n, ev.capture_secs);
    n += 4;
    out[n++] = static_cast<uint8_t>(ev.operation);
    out[n++] = static_cast<uint8_t>(ev.field_count & 0xFF);
    out[n++] = static_cast<uint8_t>((ev.field_count >> 8) & 0xFF);

    auto putStr = [&](const char *s, size_t maxLen) -> bool {
        const size_t len = strnlen(s, maxLen);
        if (!need(1 + len)) {
            return false;
        }
        out[n++] = static_cast<uint8_t>(len);
        memcpy(out + n, s, len);
        n += len;
        return true;
    };

    if (!putStr(ev.database, sizeof(ev.database) - 1) || !putStr(ev.schema, sizeof(ev.schema) - 1) ||
        !putStr(ev.table, sizeof(ev.table) - 1)) {
        return false;
    }

    for (uint16_t i = 0; i < ev.field_count; ++i) {
        const FieldMapping &f = ev.fields[i];
        const FieldValue &v = ev.values[i];
        if (!putStr(f.plc_tag, sizeof(f.plc_tag) - 1) || !putStr(f.sql_column, sizeof(f.sql_column) - 1)) {
            return false;
        }
        if (!need(6)) {
            return false;
        }
        out[n++] = static_cast<uint8_t>(f.plc_type);
        out[n++] = static_cast<uint8_t>(f.sql_type);
        out[n++] = f.required ? 1 : 0;
        out[n++] = f.nullable ? 1 : 0;
        out[n++] = f.is_where_key ? 1 : 0;
        out[n++] = v.valid ? 1 : 0;
        out[n++] = v.is_null ? 1 : 0;
        if (!need(8 + 4)) {
            return false;
        }
        W64(out + n, static_cast<uint64_t>(v.i64));
        n += 8;
        memcpy(out + n, &v.f32, 4);
        n += 4;
        if (!putStr(v.text, sizeof(v.text) - 1)) {
            return false;
        }
    }

    outLen = n;
    crc = Crc32(out, outLen);
    return true;
}

bool DeserializeEvent(const uint8_t *in, size_t inLen, CapturedEvent &ev)
{
    ev = CapturedEvent{};
    if (!in || inLen < 32) {
        return false;
    }
    size_t n = 0;
    if (R32(in + n) != 0x47574E54u) {
        return false;
    }
    n += 4;
    const uint8_t ver = in[n++];
    n++; // pad
    if (ver != 1) {
        return false;
    }
    ev.event_id = R64(in + n);
    n += 8;
    ev.plc_id = R32(in + n);
    n += 4;
    ev.mapping_id = R32(in + n);
    n += 4;
    ev.config_revision = R32(in + n);
    n += 4;
    ev.capture_secs = R32(in + n);
    n += 4;
    ev.operation = static_cast<SqlOperation>(in[n++]);
    ev.field_count = static_cast<uint16_t>(in[n] | (in[n + 1] << 8));
    n += 2;
    if (ev.field_count > kMaxFieldsPerMapping) {
        return false;
    }

    auto getStr = [&](char *dst, size_t cap) -> bool {
        if (n >= inLen) {
            return false;
        }
        const size_t len = in[n++];
        if (n + len > inLen || len >= cap) {
            return false;
        }
        memcpy(dst, in + n, len);
        dst[len] = '\0';
        n += len;
        return true;
    };

    if (!getStr(ev.database, sizeof(ev.database)) || !getStr(ev.schema, sizeof(ev.schema)) ||
        !getStr(ev.table, sizeof(ev.table))) {
        return false;
    }

    for (uint16_t i = 0; i < ev.field_count; ++i) {
        FieldMapping &f = ev.fields[i];
        FieldValue &v = ev.values[i];
        if (!getStr(f.plc_tag, sizeof(f.plc_tag)) || !getStr(f.sql_column, sizeof(f.sql_column))) {
            return false;
        }
        if (n + 7 + 8 + 4 > inLen) {
            return false;
        }
        f.plc_type = static_cast<PlcDataType>(in[n++]);
        f.sql_type = static_cast<SqlDataType>(in[n++]);
        f.required = in[n++] != 0;
        f.nullable = in[n++] != 0;
        f.is_where_key = in[n++] != 0;
        v.valid = in[n++] != 0;
        v.is_null = in[n++] != 0;
        v.i64 = static_cast<int64_t>(R64(in + n));
        n += 8;
        memcpy(&v.f32, in + n, 4);
        n += 4;
        v.plc_type = f.plc_type;
        if (!getStr(v.text, sizeof(v.text))) {
            return false;
        }
    }
    return true;
}

} // namespace gateway
