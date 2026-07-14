// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney
//
// CIP tag path encoding — from NetBurnerGateway (PLC portion of reajet_runtime.cpp).

#include <micro800/cip_path.hpp>
#include <micro800/util.hpp>

#include <cctype>
#include <cstring>

namespace micro800 {

static bool AppendCipSymbolSegment(const char *seg, uint8_t *path, size_t pathCap, size_t &pathLen)
{
    if (!seg || !seg[0]) {
        return false;
    }
    const size_t segLen = strlen(seg);
    if (segLen > 255) {
        return false;
    }
    const size_t need = 2 + segLen + ((segLen & 1) ? 1 : 0);
    if ((pathLen + need) > pathCap) {
        return false;
    }
    path[pathLen++] = 0x91;
    path[pathLen++] = static_cast<uint8_t>(segLen);
    memcpy(path + pathLen, seg, segLen);
    pathLen += segLen;
    if (segLen & 1) {
        path[pathLen++] = 0x00;
    }
    return true;
}

static bool AppendCipArrayIndex(uint32_t idx, uint8_t *path, size_t pathCap, size_t &pathLen)
{
    if (idx <= 0xFFu) {
        if ((pathLen + 2) > pathCap) {
            return false;
        }
        path[pathLen++] = 0x28;
        path[pathLen++] = static_cast<uint8_t>(idx & 0xFFu);
        return true;
    }
    if (idx <= 0xFFFFu) {
        if ((pathLen + 4) > pathCap) {
            return false;
        }
        path[pathLen++] = 0x29;
        path[pathLen++] = 0x00;
        path[pathLen++] = static_cast<uint8_t>(idx & 0xFFu);
        path[pathLen++] = static_cast<uint8_t>((idx >> 8) & 0xFFu);
        return true;
    }
    return false;
}

bool EncodeCipTagPath(const char *tagPath, uint8_t *path, size_t pathCap, size_t &pathLenOut)
{
    if (!tagPath || !tagPath[0] || !path || pathCap < 4) {
        return false;
    }
    pathLenOut = 0;
    const char *p = tagPath;
    while (*p) {
        char seg[80]{0};
        size_t s = 0;
        while (*p && *p != '.') {
            if (s + 1 >= sizeof(seg)) {
                return false;
            }
            seg[s++] = *p++;
        }
        if (*p == '.') {
            ++p;
        }
        seg[s] = '\0';
        if (!seg[0]) {
            return false;
        }

        const char *br = strchr(seg, '[');
        char base[80]{0};
        if (!br) {
            CopyString(base, sizeof(base), seg);
            if (!AppendCipSymbolSegment(base, path, pathCap, pathLenOut)) {
                return false;
            }
            continue;
        }

        const size_t baseLen = static_cast<size_t>(br - seg);
        if (baseLen == 0 || baseLen >= sizeof(base)) {
            return false;
        }
        memcpy(base, seg, baseLen);
        base[baseLen] = '\0';
        if (!AppendCipSymbolSegment(base, path, pathCap, pathLenOut)) {
            return false;
        }

        const char *q = br;
        while (*q) {
            if (*q != '[') {
                return false;
            }
            ++q;
            if (!isdigit(static_cast<unsigned char>(*q))) {
                return false;
            }
            uint32_t idx = 0;
            while (*q && isdigit(static_cast<unsigned char>(*q))) {
                idx = (idx * 10u) + static_cast<uint32_t>(*q - '0');
                ++q;
            }
            if (*q != ']') {
                return false;
            }
            ++q;
            if (!AppendCipArrayIndex(idx, path, pathCap, pathLenOut)) {
                return false;
            }
        }
    }
    return (pathLenOut > 0 && (pathLenOut % 2) == 0);
}

} // namespace micro800
