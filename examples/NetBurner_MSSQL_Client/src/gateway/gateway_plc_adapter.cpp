// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>

#include "gateway_plc_adapter.h"

#include <cstdio>
#include <cstring>

#include <Micro800Client/micro800_client.h>

#ifndef GATEWAY_PLC_SERIAL_LOG
#define GATEWAY_PLC_SERIAL_LOG 1
#endif
#if GATEWAY_PLC_SERIAL_LOG
#define PLC_FAIL_LOG(...) iprintf(__VA_ARGS__)
#else
#define PLC_FAIL_LOG(...) ((void)0)
#endif

namespace gateway {
namespace {

void SetErr(char *err, size_t cap, const char *msg)
{
    if (!err || cap < 2) {
        return;
    }
    if (!msg) {
        err[0] = '\0';
        return;
    }
    strncpy(err, msg, cap - 1);
    err[cap - 1] = '\0';
}

} // namespace

PlcDataType PlcTypeFromSymbol(uint16_t symbolType)
{
    switch (symbolType & 0xFFFu) {
    case 0x0C1:
        return PlcDataType::Bool;
    case 0x0C2:
        return PlcDataType::Sint;
    case 0x0C3:
        return PlcDataType::Int;
    case 0x0C4:
        return PlcDataType::Dint;
    case 0x0C5:
        return PlcDataType::Lint;
    case 0x0C6: // USINT
    case 0x0D1: // BYTE
        return PlcDataType::Usint;
    case 0x0C7: // UINT
    case 0x0D2: // WORD
        return PlcDataType::Uint;
    case 0x0C8: // UDINT
    case 0x0D3: // DWORD
        return PlcDataType::Udint;
    case 0x0C9: // ULINT
    case 0x0D4: // LWORD
        return PlcDataType::Ulint;
    case 0x0CA:
        return PlcDataType::Real;
    case 0x0CB:
        return PlcDataType::Lreal;
    case 0x0DA:
    case 0x0D0:
        return PlcDataType::String;
    default:
        return PlcDataType::Unknown;
    }
}

bool ProbePlc(const PlcConfig &plc, char *err, size_t errCap)
{
    return micro800::ProbePlc(plc.ip, err, errCap);
}

bool PlcReadBool(const PlcConfig &plc, const char *tag, bool &value, char *err, size_t errCap)
{
    value = false;
    micro800::TagValue tv{};
    if (!micro800::ReadTag(plc.ip, tag, tv, err, errCap)) {
        PLC_FAIL_LOG("PLC: BOOL read failed tag=%s err=%s\r\n", tag ? tag : "?",
                     (err && err[0]) ? err : "?");
        return false;
    }
    if (tv.dataLen < 1) {
        SetErr(err, errCap, "Empty BOOL read");
        PLC_FAIL_LOG("PLC: empty BOOL read tag=%s\r\n", tag ? tag : "?");
        return false;
    }
    value = (tv.data[0] != 0);
    return true;
}

bool PlcWriteBool(const PlcConfig &plc, const char *tag, bool value, char *err, size_t errCap)
{
    uint8_t b = value ? 1u : 0u;
    if (!micro800::WriteTag(plc.ip, tag, 0x0C1, &b, 1, err, errCap)) {
        PLC_FAIL_LOG("PLC: BOOL write failed tag=%s val=%u err=%s\r\n", tag ? tag : "?",
                     static_cast<unsigned>(b), (err && err[0]) ? err : "?");
        return false;
    }
    return true;
}

bool PlcReadField(const PlcConfig &plc, const FieldMapping &field, FieldValue &out, char *err, size_t errCap)
{
    out = FieldValue{};
    out.plc_type = field.plc_type;

    micro800::TagValue tv{};
    if (!micro800::ReadTag(plc.ip, field.plc_tag, tv, err, errCap)) {
        PLC_FAIL_LOG("PLC: field read failed tag=%s configured=%d err=%s\r\n",
                     field.plc_tag, static_cast<int>(field.plc_type),
                     (err && err[0]) ? err : "?");
        return false;
    }

    char text[kMaxStringValueLen]{};
    micro800::FormatTagValueText(tv.typeCode, tv.data, tv.dataLen, text, sizeof(text));

    // Prefer the live CIP type from the read so SQL conversion uses the real value shape.
    const PlcDataType live = PlcTypeFromSymbol(tv.typeCode);
    const PlcDataType inferred =
        (live != PlcDataType::Unknown) ? live
                                       : ((field.plc_type != PlcDataType::Unknown) ? field.plc_type
                                                                                  : PlcDataType::Unknown);
    out.plc_type = inferred;

    if (field.plc_type != PlcDataType::Unknown && live != PlcDataType::Unknown &&
        field.plc_type != live) {
        PLC_FAIL_LOG("PLC: type mismatch tag=%s configured=%d live_cip=0x%03X live=%d\r\n",
                     field.plc_tag, static_cast<int>(field.plc_type),
                     static_cast<unsigned>(tv.typeCode & 0xFFFu), static_cast<int>(live));
    }

    switch (inferred) {
    case PlcDataType::Bool:
        out.i64 = (tv.dataLen > 0 && tv.data[0] != 0) ? 1 : 0;
        strncpy(out.text, out.i64 ? "1" : "0", sizeof(out.text) - 1);
        break;
    case PlcDataType::Sint:
        if (tv.dataLen >= 1) {
            out.i64 = static_cast<int64_t>(static_cast<int8_t>(tv.data[0]));
        }
        sniprintf(out.text, sizeof(out.text), "%ld", static_cast<long>(out.i64));
        break;
    case PlcDataType::Usint:
        if (tv.dataLen >= 1) {
            out.i64 = static_cast<int64_t>(tv.data[0]);
        }
        sniprintf(out.text, sizeof(out.text), "%lu", static_cast<unsigned long>(out.i64));
        break;
    case PlcDataType::Int:
        if (tv.dataLen >= 2) {
            const uint16_t raw = static_cast<uint16_t>(tv.data[0] | (tv.data[1] << 8));
            out.i64 = static_cast<int64_t>(static_cast<int16_t>(raw));
        }
        sniprintf(out.text, sizeof(out.text), "%ld", static_cast<long>(out.i64));
        break;
    case PlcDataType::Uint:
        if (tv.dataLen >= 2) {
            out.i64 = static_cast<int64_t>(
                static_cast<uint16_t>(tv.data[0] | (tv.data[1] << 8)));
        }
        sniprintf(out.text, sizeof(out.text), "%lu", static_cast<unsigned long>(out.i64));
        break;
    case PlcDataType::Dint:
        if (tv.dataLen >= 4) {
            const uint32_t raw = static_cast<uint32_t>(tv.data[0] | (tv.data[1] << 8) |
                                                      (tv.data[2] << 16) | (tv.data[3] << 24));
            out.i64 = static_cast<int64_t>(static_cast<int32_t>(raw));
        }
        sniprintf(out.text, sizeof(out.text), "%ld", static_cast<long>(out.i64));
        break;
    case PlcDataType::Udint:
        if (tv.dataLen >= 4) {
            out.i64 = static_cast<int64_t>(
                static_cast<uint32_t>(tv.data[0] | (tv.data[1] << 8) | (tv.data[2] << 16) |
                                     (tv.data[3] << 24)));
        }
        sniprintf(out.text, sizeof(out.text), "%lu", static_cast<unsigned long>(out.i64));
        break;
    case PlcDataType::Lint:
        if (tv.dataLen >= 8) {
            uint64_t raw = 0;
            for (int i = 0; i < 8; ++i) {
                raw |= static_cast<uint64_t>(tv.data[i]) << (8 * i);
            }
            out.i64 = static_cast<int64_t>(raw);
        }
        sniprintf(out.text, sizeof(out.text), "%lld", static_cast<long long>(out.i64));
        break;
    case PlcDataType::Ulint:
        if (tv.dataLen >= 8) {
            uint64_t raw = 0;
            for (int i = 0; i < 8; ++i) {
                raw |= static_cast<uint64_t>(tv.data[i]) << (8 * i);
            }
            // Store full range in i64 bit pattern; text uses unsigned formatting.
            out.i64 = static_cast<int64_t>(raw);
        }
        sniprintf(out.text, sizeof(out.text), "%llu",
                  static_cast<unsigned long long>(out.i64));
        break;
    case PlcDataType::Real:
        if (tv.dataLen >= 4) {
            memcpy(&out.f32, tv.data, 4);
            out.i64 = static_cast<int64_t>(out.f32);
        }
        sniprintf(out.text, sizeof(out.text), "%f", static_cast<double>(out.f32));
        break;
    case PlcDataType::Lreal:
        if (tv.dataLen >= 8) {
            double d = 0.0;
            memcpy(&d, tv.data, 8);
            out.f32 = static_cast<float>(d);
            out.i64 = static_cast<int64_t>(d);
            sniprintf(out.text, sizeof(out.text), "%f", d);
        } else if (tv.dataLen >= 4) {
            memcpy(&out.f32, tv.data, 4);
            sniprintf(out.text, sizeof(out.text), "%f", static_cast<double>(out.f32));
        }
        break;
    case PlcDataType::String:
    default:
        strncpy(out.text, text, sizeof(out.text) - 1);
        break;
    }

    if (field.scale != 1.0 || field.offset != 0.0) {
        if (inferred == PlcDataType::Real || inferred == PlcDataType::Lreal) {
            out.f32 = static_cast<float>(out.f32 * field.scale + field.offset);
            sniprintf(out.text, sizeof(out.text), "%f", static_cast<double>(out.f32));
        } else if (inferred != PlcDataType::String && inferred != PlcDataType::Bool) {
            const double scaled = static_cast<double>(out.i64) * field.scale + field.offset;
            out.i64 = static_cast<int64_t>(scaled);
            sniprintf(out.text, sizeof(out.text), "%lld", static_cast<long long>(out.i64));
        }
    }

    out.valid = true;
    return true;
}

} // namespace gateway
