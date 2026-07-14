// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney

#ifndef MICRO800_CIP_TAG_IO_HPP
#define MICRO800_CIP_TAG_IO_HPP

#include <cstddef>
#include <cstdint>

#include <nettypes.h>

#include <micro800/types.hpp>

namespace micro800 {

// TCP connect probe to ENIP port 44818 (opens and closes immediately).
bool ProbePlc(IPADDR4 ip, char *errorOut = nullptr, size_t errorOutSize = 0);

// Each call opens a new ENIP session, performs one CIP request, then closes.
bool ReadTag(IPADDR4 ip,
             const char *tagPath,
             TagValue &valueOut,
             char *errorOut = nullptr,
             size_t errorOutSize = 0);

bool ReadTagRaw(IPADDR4 ip,
                const char *tagPath,
                uint16_t &typeCodeOut,
                uint8_t *dataOut,
                size_t dataCap,
                size_t &dataLenOut,
                char *errorOut = nullptr,
                size_t errorOutSize = 0);

bool WriteTag(IPADDR4 ip,
              const char *tagPath,
              uint16_t typeCode,
              const uint8_t *data,
              size_t dataLen,
              char *errorOut = nullptr,
              size_t errorOutSize = 0);

// CIP List Tags (0x55) on Symbol object 0x6B. Fills caller-owned buffer.
// Returns number of tags written to out (0 on failure).
int BrowseTags(IPADDR4 ip,
               BrowsedTag *out,
               size_t capacity,
               char *errorOut = nullptr,
               size_t errorOutSize = 0);

} // namespace micro800

#endif
