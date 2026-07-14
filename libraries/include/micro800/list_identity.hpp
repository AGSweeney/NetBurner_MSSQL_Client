// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney

#ifndef MICRO800_LIST_IDENTITY_HPP
#define MICRO800_LIST_IDENTITY_HPP

#include <cstddef>
#include <cstdint>

#include <micro800/types.hpp>

namespace micro800 {

bool LooksLikeMicro800(uint16_t vendorId, const char *productName);

bool LooksLikeMicro800(const DiscoveredDevice &device);

// UDP ListIdentity broadcast scan. Fills caller-owned buffer.
// Returns number of devices found (0 on failure).
int ScanListIdentity(DiscoveredDevice *out, size_t capacity, char *errorOut = nullptr, size_t errorOutSize = 0);

} // namespace micro800

#endif
