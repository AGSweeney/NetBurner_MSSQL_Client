// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney

#ifndef MICRO800_TAG_FORMAT_HPP
#define MICRO800_TAG_FORMAT_HPP

#include <cstddef>
#include <cstdint>

namespace micro800 {

void FormatTagValueText(uint16_t typeCode, const uint8_t *data, size_t len, char *out, size_t outSize);

} // namespace micro800

#endif
