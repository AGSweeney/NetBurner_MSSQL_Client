// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney

#ifndef MICRO800_UTIL_HPP
#define MICRO800_UTIL_HPP

#include <cstddef>
#include <cstdint>

namespace micro800 {

void SetError(char *errorOut, size_t errorOutSize, const char *msg);

bool IsAllowedTagChar(char c);

bool NormalizeTagPath(const char *input, char *out, size_t outSize);

bool ContainsNoCase(const char *text, const char *token);

void CopyString(char *dst, size_t dstSize, const char *src);

void HexEncode(const uint8_t *data, size_t len, char *out, size_t outSize);

} // namespace micro800

#endif
