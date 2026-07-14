// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney

#include <micro800/util.hpp>

#include <cctype>
#include <cstdio>
#include <cstring>

#include <stdio.h> // sniprintf

namespace micro800 {

void SetError(char *errorOut, size_t errorOutSize, const char *msg)
{
    if (!errorOut || errorOutSize < 2) {
        return;
    }
    if (!msg) {
        errorOut[0] = '\0';
        return;
    }
    const size_t len = strlen(msg);
    const size_t copyLen = (len < (errorOutSize - 1)) ? len : (errorOutSize - 1);
    memcpy(errorOut, msg, copyLen);
    errorOut[copyLen] = '\0';
}

bool IsAllowedTagChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '.' || c == '[' || c == ']' || c == ':';
}

bool NormalizeTagPath(const char *input, char *out, size_t outSize)
{
    if (!input || !out || outSize < 2) {
        return false;
    }

    while (*input == ' ' || *input == '\t') {
        ++input;
    }

    size_t n = 0;
    while (*input && n + 1 < outSize) {
        if (*input != ' ' && *input != '\t' && *input != '\r' && *input != '\n') {
            if (!IsAllowedTagChar(*input)) {
                return false;
            }
            out[n++] = *input;
        }
        ++input;
    }
    out[n] = '\0';
    return n > 0;
}

bool ContainsNoCase(const char *text, const char *token)
{
    if (!text || !token || !token[0]) {
        return false;
    }

    const size_t textLen = strlen(text);
    const size_t tokenLen = strlen(token);
    if (tokenLen > textLen) {
        return false;
    }

    for (size_t i = 0; i + tokenLen <= textLen; ++i) {
        size_t j = 0;
        for (; j < tokenLen; ++j) {
            const unsigned char a = static_cast<unsigned char>(text[i + j]);
            const unsigned char b = static_cast<unsigned char>(token[j]);
            if (std::tolower(a) != std::tolower(b)) {
                break;
            }
        }
        if (j == tokenLen) {
            return true;
        }
    }
    return false;
}

void CopyString(char *dst, size_t dstSize, const char *src)
{
    if (!dst || dstSize == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }
    sniprintf(dst, dstSize, "%s", src);
}

void HexEncode(const uint8_t *data, size_t len, char *out, size_t outSize)
{
    if (!out || outSize < 2) {
        return;
    }
    out[0] = '\0';
    if (!data || len == 0) {
        return;
    }
    static const char kHex[] = "0123456789ABCDEF";
    size_t w = 0;
    for (size_t i = 0; i < len && (w + 2) < outSize; ++i) {
        const uint8_t b = data[i];
        out[w++] = kHex[(b >> 4) & 0x0F];
        out[w++] = kHex[b & 0x0F];
    }
    out[w] = '\0';
}

} // namespace micro800
