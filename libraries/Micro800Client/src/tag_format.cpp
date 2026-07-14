// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney
//
// Tag value formatting — from NetBurnerGateway FormatTagValueText.

#include <micro800/tag_format.hpp>
#include <micro800/endian.hpp>
#include <micro800/util.hpp>

#include <cstring>

#include <stdio.h> // sniprintf

namespace micro800 {

void FormatTagValueText(uint16_t typeCode, const uint8_t *data, size_t len, char *out, size_t outSize)
{
    if (!out || outSize < 2) {
        return;
    }
    out[0] = '\0';
    if (!data || len == 0) {
        return;
    }

    switch (typeCode) {
    case 0xC1:
        sniprintf(out, outSize, "%u", data[0] ? 1u : 0u);
        return;
    case 0xC2:
        sniprintf(out, outSize, "%d", static_cast<int>(static_cast<int8_t>(data[0])));
        return;
    case 0xC6:
        sniprintf(out, outSize, "%u", static_cast<unsigned>(data[0]));
        return;
    case 0xC3:
    case 0xC7:
        if (len >= 2) {
            const uint16_t v = ReadLe16(data);
            if (typeCode == 0xC3)
                sniprintf(out, outSize, "%d", static_cast<int>(static_cast<int16_t>(v)));
            else
                sniprintf(out, outSize, "%u", static_cast<unsigned>(v));
            return;
        }
        break;
    case 0xC4:
    case 0xC8:
        if (len >= 4) {
            const uint32_t v = ReadLe32(data);
            if (typeCode == 0xC4)
                sniprintf(out, outSize, "%ld", static_cast<long>(static_cast<int32_t>(v)));
            else
                sniprintf(out, outSize, "%lu", static_cast<unsigned long>(v));
            return;
        }
        break;
    case 0xCA:
        if (len >= 4) {
            float f = 0.0f;
            memcpy(&f, data, 4);
            sniprintf(out, outSize, "%.6f", static_cast<double>(f));
            return;
        }
        break;
    case 0xDA: {
        const uint8_t slen = data[0];
        size_t n = static_cast<size_t>(slen);
        if ((1 + n) > len) {
            n = (len > 1) ? (len - 1) : 0;
        }
        if (n >= outSize) {
            n = outSize - 1;
        }
        memcpy(out, data + 1, n);
        out[n] = '\0';
        // Micro800 STRING buffers are often space-padded to max length.
        while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\0')) {
            out[--n] = '\0';
        }
        return;
    }
    default:
        break;
    }

    HexEncode(data, len, out, outSize);
}

} // namespace micro800
