// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney
//
// Micro800 / EtherNet/IP client types.
// Protocol logic sourced from NetBurnerGateway CIP Data Gateway (Reajet excluded).

#ifndef MICRO800_TYPES_HPP
#define MICRO800_TYPES_HPP

#include <cstddef>
#include <cstdint>

#include <nettypes.h>

namespace micro800 {

static const int kMaxDiscoveredDevices = 16;
static const int kMaxBrowsedTags = 512;
static const int kMaxTagPathLen = 96;
static const uint16_t kEnipPort = 44818;

struct DiscoveredDevice {
    IPADDR4 ip{};
    uint16_t vendorId{0};
    uint16_t deviceType{0};
    uint16_t productCode{0};
    uint8_t majorRev{0};
    uint8_t minorRev{0};
    uint32_t serial{0};
    char productName[33]{0};
    bool micro800{false};
    bool valid{false};
};

struct BrowsedTag {
    char name[kMaxTagPathLen]{0};
    uint16_t symbolType{0};
    uint16_t elementSize{0};
    uint32_t dim0{0};
    uint32_t dim1{0};
    uint32_t dim2{0};
    bool isArray{false};
    uint32_t arrayLength{0};
};

struct TagValue {
    uint16_t typeCode{0};
    uint8_t data[256]{0};
    size_t dataLen{0};
};

} // namespace micro800

#endif
