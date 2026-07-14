// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>

#ifndef GATEWAY_SERIALIZER_H
#define GATEWAY_SERIALIZER_H

#include "gateway_types.h"

namespace gateway {

bool SerializeEvent(const CapturedEvent &ev, uint8_t *out, size_t outCap, size_t &outLen, uint32_t &crc);
bool DeserializeEvent(const uint8_t *in, size_t inLen, CapturedEvent &ev);

uint32_t Crc32(const uint8_t *data, size_t len);

} // namespace gateway

#endif
