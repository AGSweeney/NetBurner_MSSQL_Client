// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney
//
// ENIP session framing — from NetBurnerGateway enip_runtime.cpp.

#include <micro800/enip_session.hpp>
#include <micro800/endian.hpp>

#include <cstring>

#include <iosys.h>
#include <nbrtos.h>
#include <tcp.h>

namespace micro800 {

bool SendAllFd(int fd, const uint8_t *data, size_t len)
{
    static const size_t kIoChunk = 32767;
    size_t sent = 0;
    while (sent < len) {
        const size_t remaining = len - sent;
        const size_t chunk = (remaining < kIoChunk) ? remaining : kIoChunk;
        const int rc = write(fd, reinterpret_cast<const char *>(data + sent), static_cast<int>(chunk));
        if (rc <= 0) {
            return false;
        }
        sent += static_cast<size_t>(rc);
    }
    return true;
}

bool RecvExactFd(int fd, uint8_t *out, size_t len, uint32_t waitTicks)
{
    static const size_t kIoChunk = 32767;
    size_t got = 0;
    while (got < len) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(fd, &readSet);
        const int ready = select(fd + 1, &readSet, nullptr, nullptr, waitTicks);
        if (ready <= 0) {
            return false;
        }
        const size_t remaining = len - got;
        const size_t chunk = (remaining < kIoChunk) ? remaining : kIoChunk;
        const int rc = read(fd, reinterpret_cast<char *>(out + got), static_cast<int>(chunk));
        if (rc <= 0) {
            return false;
        }
        got += static_cast<size_t>(rc);
    }
    return true;
}

bool RecvEnipFrame(int fd,
                   uint16_t &command,
                   uint16_t &payloadLen,
                   uint32_t &sessionHandle,
                   uint32_t &status,
                   uint8_t *payload,
                   size_t payloadCap)
{
    uint8_t header[24]{0};
    if (!RecvExactFd(fd, header, sizeof(header), TICKS_PER_SECOND * 2)) {
        return false;
    }
    command = ReadLe16(header + 0);
    payloadLen = ReadLe16(header + 2);
    sessionHandle = ReadLe32(header + 4);
    status = ReadLe32(header + 8);
    if (payloadLen > payloadCap) {
        return false;
    }
    if (payloadLen == 0) {
        return true;
    }
    return RecvExactFd(fd, payload, payloadLen, TICKS_PER_SECOND * 2);
}

bool RegisterEnipSessionFd(int fd, uint32_t &sessionHandleOut)
{
    static const uint16_t kEnipRegisterSession = 0x0065;
    uint8_t req[28]{0};
    WriteLe16(req + 0, kEnipRegisterSession);
    WriteLe16(req + 2, 4);
    req[24] = 0x01;
    req[25] = 0x00;

    if (!SendAllFd(fd, req, sizeof(req))) {
        return false;
    }

    uint16_t cmd = 0;
    uint16_t payloadLen = 0;
    uint32_t session = 0;
    uint32_t status = 0;
    uint8_t payload[64]{0};
    if (!RecvEnipFrame(fd, cmd, payloadLen, session, status, payload, sizeof(payload))) {
        return false;
    }
    if (cmd != kEnipRegisterSession || status != 0 || payloadLen < 4 || session == 0) {
        return false;
    }
    sessionHandleOut = session;
    return true;
}

void UnregisterEnipSessionFd(int fd, uint32_t sessionHandle)
{
    static const uint16_t kEnipUnregisterSession = 0x0066;
    uint8_t req[24]{0};
    WriteLe16(req + 0, kEnipUnregisterSession);
    WriteLe32(req + 4, sessionHandle);
    SendAllFd(fd, req, sizeof(req));
}

bool SendEnipRrData(int fd,
                    uint32_t sessionHandle,
                    const uint8_t *cip,
                    size_t cipLen,
                    uint8_t *rrPayload,
                    size_t rrPayloadCap,
                    size_t &rrPayloadLenOut)
{
    static const uint16_t kEnipSendRrData = 0x006F;
    static const uint16_t kCpfDataItem = 0x00B2;
    if (!cip || cipLen == 0 || cipLen > 600) {
        return false;
    }

    const uint16_t rrLen = static_cast<uint16_t>(16 + cipLen);
    uint8_t packet[700]{0};
    size_t o = 0;
    WriteLe16(packet + o, kEnipSendRrData);
    o += 2;
    WriteLe16(packet + o, rrLen);
    o += 2;
    WriteLe32(packet + o, sessionHandle);
    o += 4;
    WriteLe32(packet + o, 0);
    o += 4; // status
    WriteLe32(packet + o, 0);
    o += 4; // sender context [0..3]
    WriteLe32(packet + o, 0);
    o += 4; // sender context [4..7]
    WriteLe32(packet + o, 0);
    o += 4; // options
    WriteLe32(packet + o, 0);
    o += 4; // interface handle
    WriteLe16(packet + o, 1);
    o += 2; // timeout
    WriteLe16(packet + o, 2);
    o += 2; // item count
    WriteLe16(packet + o, 0x0000);
    o += 2; // null address item
    WriteLe16(packet + o, 0x0000);
    o += 2;
    WriteLe16(packet + o, kCpfDataItem);
    o += 2;
    WriteLe16(packet + o, static_cast<uint16_t>(cipLen));
    o += 2;
    memcpy(packet + o, cip, cipLen);
    o += cipLen;

    if (!SendAllFd(fd, packet, o)) {
        return false;
    }

    uint16_t cmd = 0;
    uint16_t payloadLen = 0;
    uint32_t session = 0;
    uint32_t status = 0;
    if (!RecvEnipFrame(fd, cmd, payloadLen, session, status, rrPayload, rrPayloadCap)) {
        return false;
    }
    if (cmd != kEnipSendRrData || status != 0 || session != sessionHandle) {
        return false;
    }
    rrPayloadLenOut = payloadLen;
    return true;
}

bool ExtractCipFromRrPayload(const uint8_t *rrPayload, size_t rrLen, const uint8_t *&cipOut, size_t &cipLenOut)
{
    if (!rrPayload || rrLen < 8) {
        return false;
    }
    size_t offset = 0;
    offset += 4; // interface handle
    offset += 2; // timeout
    const uint16_t itemCount = ReadLe16(rrPayload + offset);
    offset += 2;
    for (uint16_t i = 0; i < itemCount; ++i) {
        if (offset + 4 > rrLen) {
            return false;
        }
        const uint16_t itemType = ReadLe16(rrPayload + offset);
        const uint16_t itemLen = ReadLe16(rrPayload + offset + 2);
        offset += 4;
        if (offset + itemLen > rrLen) {
            return false;
        }
        if (itemType == 0x00B2) {
            cipOut = rrPayload + offset;
            cipLenOut = itemLen;
            return true;
        }
        offset += itemLen;
    }
    return false;
}

} // namespace micro800
