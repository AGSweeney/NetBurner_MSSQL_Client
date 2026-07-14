// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney

#ifndef MICRO800_ENIP_SESSION_HPP
#define MICRO800_ENIP_SESSION_HPP

#include <cstddef>
#include <cstdint>

namespace micro800 {

bool SendAllFd(int fd, const uint8_t *data, size_t len);

bool RecvExactFd(int fd, uint8_t *out, size_t len, uint32_t waitTicks);

bool RecvEnipFrame(int fd,
                   uint16_t &command,
                   uint16_t &payloadLen,
                   uint32_t &sessionHandle,
                   uint32_t &status,
                   uint8_t *payload,
                   size_t payloadCap);

bool RegisterEnipSessionFd(int fd, uint32_t &sessionHandleOut);

void UnregisterEnipSessionFd(int fd, uint32_t sessionHandle);

bool SendEnipRrData(int fd,
                    uint32_t sessionHandle,
                    const uint8_t *cip,
                    size_t cipLen,
                    uint8_t *rrPayload,
                    size_t rrPayloadCap,
                    size_t &rrPayloadLenOut);

bool ExtractCipFromRrPayload(const uint8_t *rrPayload,
                             size_t rrLen,
                             const uint8_t *&cipOut,
                             size_t &cipLenOut);

} // namespace micro800

#endif
