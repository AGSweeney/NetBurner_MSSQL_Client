// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney

#ifndef MICRO800_CIP_PATH_HPP
#define MICRO800_CIP_PATH_HPP

#include <cstddef>
#include <cstdint>

namespace micro800 {

bool EncodeCipTagPath(const char *tagPath, uint8_t *path, size_t pathCap, size_t &pathLenOut);

} // namespace micro800

#endif
