// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney
//
// CIP tag read/write/browse — from NetBurnerGateway (PLC portion; Reajet excluded).

#include <micro800/cip_tag_io.hpp>
#include <micro800/cip_path.hpp>
#include <micro800/endian.hpp>
#include <micro800/enip_session.hpp>
#include <micro800/util.hpp>

#include <cstring>

#include <iosys.h>
#include <nbrtos.h>
#include <tcp.h>

namespace micro800 {

bool ProbePlc(IPADDR4 ip, char *errorOut, size_t errorOutSize)
{
    SetError(errorOut, errorOutSize, "");
    if (ip.IsNull()) {
        SetError(errorOut, errorOutSize, "No PLC IP");
        return false;
    }

    const int fd = connect(ip, kEnipPort, TICKS_PER_SECOND * 3);
    if (fd < 0) {
        SetError(errorOut, errorOutSize, "TCP connect to 44818 failed");
        return false;
    }
    close(fd);
    return true;
}

bool ReadTagRaw(IPADDR4 ip,
                const char *tagPath,
                uint16_t &typeCodeOut,
                uint8_t *dataOut,
                size_t dataCap,
                size_t &dataLenOut,
                char *errorOut,
                size_t errorOutSize)
{
    SetError(errorOut, errorOutSize, "");
    dataLenOut = 0;
    typeCodeOut = 0;

    if (ip.IsNull()) {
        SetError(errorOut, errorOutSize, "No PLC IP");
        return false;
    }
    if (!tagPath || !tagPath[0] || !dataOut || dataCap == 0) {
        SetError(errorOut, errorOutSize, "Invalid read parameters");
        return false;
    }

    uint8_t tagPathCip[256]{0};
    size_t tagPathLen = 0;
    if (!EncodeCipTagPath(tagPath, tagPathCip, sizeof(tagPathCip), tagPathLen)) {
        SetError(errorOut, errorOutSize, "Invalid tag path");
        return false;
    }

    const int fd = connect(ip, kEnipPort, TICKS_PER_SECOND * 3);
    if (fd < 0) {
        SetError(errorOut, errorOutSize, "TCP connect to PLC failed");
        return false;
    }

    uint32_t session = 0;
    if (!RegisterEnipSessionFd(fd, session)) {
        close(fd);
        SetError(errorOut, errorOutSize, "ENIP RegisterSession failed");
        return false;
    }

    uint8_t cip[512]{0};
    size_t c = 0;
    cip[c++] = 0x4C; // CIP Read Tag service
    cip[c++] = static_cast<uint8_t>(tagPathLen / 2);
    memcpy(cip + c, tagPathCip, tagPathLen);
    c += tagPathLen;
    WriteLe16(cip + c, 1); // element count
    c += 2;

    uint8_t rrPayload[1024]{0};
    size_t rrLen = 0;
    if (!SendEnipRrData(fd, session, cip, c, rrPayload, sizeof(rrPayload), rrLen)) {
        UnregisterEnipSessionFd(fd, session);
        close(fd);
        SetError(errorOut, errorOutSize, "ENIP SendRRData failed");
        return false;
    }

    const uint8_t *cipResp = nullptr;
    size_t cipRespLen = 0;
    if (!ExtractCipFromRrPayload(rrPayload, rrLen, cipResp, cipRespLen) || cipRespLen < 4) {
        UnregisterEnipSessionFd(fd, session);
        close(fd);
        SetError(errorOut, errorOutSize, "Invalid CIP payload");
        return false;
    }

    const uint8_t service = cipResp[0];
    const uint8_t status = cipResp[2];
    const uint8_t addStatusWords = cipResp[3];
    const size_t payloadOffset = 4 + (static_cast<size_t>(addStatusWords) * 2);
    if (service != 0xCC || payloadOffset > cipRespLen) {
        UnregisterEnipSessionFd(fd, session);
        close(fd);
        SetError(errorOut, errorOutSize, "Unexpected read-tag response");
        return false;
    }
    if (status != 0) {
        UnregisterEnipSessionFd(fd, session);
        close(fd);
        SetError(errorOut, errorOutSize, "PLC read-tag returned error");
        return false;
    }

    const uint8_t *payload = cipResp + payloadOffset;
    const size_t payloadLen = cipRespLen - payloadOffset;
    if (payloadLen < 2) {
        UnregisterEnipSessionFd(fd, session);
        close(fd);
        SetError(errorOut, errorOutSize, "Read-tag payload too short");
        return false;
    }

    typeCodeOut = ReadLe16(payload);
    const size_t rawLen = payloadLen - 2;
    dataLenOut = (rawLen < dataCap) ? rawLen : dataCap;
    if (dataLenOut > 0) {
        memcpy(dataOut, payload + 2, dataLenOut);
    }

    UnregisterEnipSessionFd(fd, session);
    close(fd);
    return true;
}

bool ReadTag(IPADDR4 ip, const char *tagPath, TagValue &valueOut, char *errorOut, size_t errorOutSize)
{
    valueOut.typeCode = 0;
    valueOut.dataLen = 0;
    memset(valueOut.data, 0, sizeof(valueOut.data));
    return ReadTagRaw(ip, tagPath, valueOut.typeCode, valueOut.data, sizeof(valueOut.data), valueOut.dataLen,
                      errorOut, errorOutSize);
}

bool WriteTag(IPADDR4 ip,
              const char *tagPath,
              uint16_t typeCode,
              const uint8_t *data,
              size_t dataLen,
              char *errorOut,
              size_t errorOutSize)
{
    SetError(errorOut, errorOutSize, "");
    if (ip.IsNull()) {
        SetError(errorOut, errorOutSize, "No PLC IP");
        return false;
    }
    if (!tagPath || !tagPath[0] || !data || dataLen == 0) {
        SetError(errorOut, errorOutSize, "Invalid write parameters");
        return false;
    }

    uint8_t tagPathCip[256]{0};
    size_t tagPathLen = 0;
    if (!EncodeCipTagPath(tagPath, tagPathCip, sizeof(tagPathCip), tagPathLen)) {
        SetError(errorOut, errorOutSize, "Invalid tag path");
        return false;
    }

    const int fd = connect(ip, kEnipPort, TICKS_PER_SECOND * 3);
    if (fd < 0) {
        SetError(errorOut, errorOutSize, "TCP connect to PLC failed");
        return false;
    }

    uint32_t session = 0;
    if (!RegisterEnipSessionFd(fd, session)) {
        close(fd);
        SetError(errorOut, errorOutSize, "ENIP RegisterSession failed");
        return false;
    }

    uint8_t cip[512]{0};
    size_t c = 0;
    if (c + 1 + 1 + tagPathLen + 2 + 2 + dataLen > sizeof(cip)) {
        UnregisterEnipSessionFd(fd, session);
        close(fd);
        SetError(errorOut, errorOutSize, "Write payload too large");
        return false;
    }
    cip[c++] = 0x4D; // CIP Write Tag service
    cip[c++] = static_cast<uint8_t>(tagPathLen / 2);
    memcpy(cip + c, tagPathCip, tagPathLen);
    c += tagPathLen;
    WriteLe16(cip + c, typeCode);
    c += 2;
    WriteLe16(cip + c, 1); // element count
    c += 2;
    memcpy(cip + c, data, dataLen);
    c += dataLen;

    uint8_t rrPayload[512]{0};
    size_t rrLen = 0;
    if (!SendEnipRrData(fd, session, cip, c, rrPayload, sizeof(rrPayload), rrLen)) {
        UnregisterEnipSessionFd(fd, session);
        close(fd);
        SetError(errorOut, errorOutSize, "ENIP SendRRData failed");
        return false;
    }

    const uint8_t *cipResp = nullptr;
    size_t cipRespLen = 0;
    if (!ExtractCipFromRrPayload(rrPayload, rrLen, cipResp, cipRespLen) || cipRespLen < 4) {
        UnregisterEnipSessionFd(fd, session);
        close(fd);
        SetError(errorOut, errorOutSize, "Invalid CIP payload");
        return false;
    }

    const uint8_t service = cipResp[0];
    const uint8_t status = cipResp[2];
    if (service != 0xCD || status != 0) {
        UnregisterEnipSessionFd(fd, session);
        close(fd);
        SetError(errorOut, errorOutSize, "PLC write-tag returned error");
        return false;
    }

    UnregisterEnipSessionFd(fd, session);
    close(fd);
    return true;
}

int BrowseTags(IPADDR4 ip, BrowsedTag *out, size_t capacity, char *errorOut, size_t errorOutSize)
{
    static const uint8_t kServiceListTags = 0x55;
    static const uint8_t kCipStatusSuccess = 0x00;
    static const uint8_t kCipStatusPartial = 0x06;

    SetError(errorOut, errorOutSize, "");
    if (!out || capacity == 0) {
        SetError(errorOut, errorOutSize, "No output buffer");
        return 0;
    }
    if (ip.IsNull()) {
        SetError(errorOut, errorOutSize, "No PLC IP");
        return 0;
    }
    if (capacity > static_cast<size_t>(kMaxBrowsedTags)) {
        capacity = static_cast<size_t>(kMaxBrowsedTags);
    }
    for (size_t i = 0; i < capacity; ++i) {
        out[i] = BrowsedTag{};
    }

    const int fd = connect(ip, kEnipPort, TICKS_PER_SECOND * 5);
    if (fd < 0) {
        SetError(errorOut, errorOutSize, "TCP connect to PLC failed");
        return 0;
    }

    uint32_t sessionHandle = 0;
    if (!RegisterEnipSessionFd(fd, sessionHandle)) {
        SetError(errorOut, errorOutSize, "ENIP RegisterSession failed");
        close(fd);
        return 0;
    }

    size_t tagCount = 0;
    uint16_t nextInstance = 0;
    int pageGuard = 0;
    bool done = false;
    while (!done && pageGuard < 256 && tagCount < capacity) {
        ++pageGuard;
        uint8_t path[8]{0};
        size_t pathLen = 0;
        path[pathLen++] = 0x20; // class
        path[pathLen++] = 0x6B; // Symbol object
        path[pathLen++] = 0x25; // 16-bit instance segment
        path[pathLen++] = 0x00;
        path[pathLen++] = static_cast<uint8_t>(nextInstance & 0xFF);
        path[pathLen++] = static_cast<uint8_t>((nextInstance >> 8) & 0xFF);

        uint8_t cip[64]{0};
        size_t c = 0;
        cip[c++] = kServiceListTags;
        cip[c++] = static_cast<uint8_t>(pathLen / 2);
        memcpy(cip + c, path, pathLen);
        c += pathLen;
        WriteLe16(cip + c, 4);
        c += 2; // attribute count
        WriteLe16(cip + c, 0x0002);
        c += 2; // symbol type
        WriteLe16(cip + c, 0x0007);
        c += 2; // dimensions
        WriteLe16(cip + c, 0x0008);
        c += 2; // dimensions
        WriteLe16(cip + c, 0x0001);
        c += 2; // name

        uint8_t rrPayload[1200]{0};
        size_t rrLen = 0;
        if (!SendEnipRrData(fd, sessionHandle, cip, c, rrPayload, sizeof(rrPayload), rrLen)) {
            SetError(errorOut, errorOutSize, "ENIP SendRRData failed");
            break;
        }

        const uint8_t *cipResp = nullptr;
        size_t cipRespLen = 0;
        if (!ExtractCipFromRrPayload(rrPayload, rrLen, cipResp, cipRespLen) || cipRespLen < 4) {
            SetError(errorOut, errorOutSize, "Invalid CIP payload");
            break;
        }

        const uint8_t service = cipResp[0];
        const uint8_t status = cipResp[2];
        const uint8_t addStatusWords = cipResp[3];
        const size_t payloadOffset = 4 + (static_cast<size_t>(addStatusWords) * 2);
        if (service != static_cast<uint8_t>(kServiceListTags | 0x80) || payloadOffset > cipRespLen) {
            SetError(errorOut, errorOutSize, "Unexpected list-tags response");
            break;
        }

        const uint8_t *data = cipResp + payloadOffset;
        const size_t dataLen = cipRespLen - payloadOffset;
        size_t offset = 0;
        uint32_t lastInstanceSeen = nextInstance;
        int parsed = 0;

        while (offset + 24 <= dataLen && tagCount < capacity) {
            const uint32_t instance = ReadLe32(data + offset);
            offset += 4;
            const uint16_t symbolType = ReadLe16(data + offset);
            offset += 2;
            const uint16_t elementLength = ReadLe16(data + offset);
            offset += 2;
            const uint32_t dim0 = ReadLe32(data + offset);
            const uint32_t dim1 = ReadLe32(data + offset + 4);
            const uint32_t dim2 = ReadLe32(data + offset + 8);
            offset += 12;
            const uint16_t nameLen = ReadLe16(data + offset);
            offset += 2;
            if (nameLen == 0 || offset + nameLen > dataLen) {
                SetError(errorOut, errorOutSize, "Tag parse failed");
                done = true;
                break;
            }

            char name[kMaxTagPathLen]{0};
            size_t n = 0;
            for (uint16_t i = 0; i < nameLen && n + 1 < sizeof(name); ++i) {
                char ch = static_cast<char>(data[offset + i]);
                if (IsAllowedTagChar(ch)) {
                    name[n++] = ch;
                } else {
                    name[n++] = '_';
                }
            }
            name[n] = '\0';
            offset += nameLen;
            ++parsed;
            lastInstanceSeen = instance;

            if (name[0] == '\0') {
                continue;
            }

            bool exists = false;
            for (size_t i = 0; i < tagCount; ++i) {
                if (strcmp(out[i].name, name) == 0) {
                    exists = true;
                    break;
                }
            }
            if (exists) {
                continue;
            }

            BrowsedTag &tag = out[tagCount++];
            memset(tag.name, 0, sizeof(tag.name));
            const size_t nameCopy = strlen(name);
            const size_t safeCopy = (nameCopy < (sizeof(tag.name) - 1)) ? nameCopy : (sizeof(tag.name) - 1);
            memcpy(tag.name, name, safeCopy);
            tag.symbolType = symbolType;
            tag.elementSize = elementLength;
            tag.dim0 = dim0;
            tag.dim1 = dim1;
            tag.dim2 = dim2;
            const uint64_t d0 = (dim0 == 0) ? 1 : dim0;
            const uint64_t d1 = (dim1 == 0) ? 1 : dim1;
            const uint64_t d2 = (dim2 == 0) ? 1 : dim2;
            const uint64_t total = d0 * d1 * d2;
            tag.isArray = (total > 1);
            tag.arrayLength = (total > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(total);
        }

        if (status == kCipStatusPartial && parsed > 0) {
            nextInstance = static_cast<uint16_t>(lastInstanceSeen + 1);
            continue;
        }
        if (status == kCipStatusSuccess && parsed > 0 && lastInstanceSeen >= nextInstance) {
            nextInstance = static_cast<uint16_t>(lastInstanceSeen + 1);
            continue;
        }
        if (status == kCipStatusSuccess) {
            done = true;
            continue;
        }

        SetError(errorOut, errorOutSize, "PLC returned list-tags error");
        done = true;
    }

    UnregisterEnipSessionFd(fd, sessionHandle);
    close(fd);
    if (tagCount > 0) {
        SetError(errorOut, errorOutSize, "");
    }
    return static_cast<int>(tagCount);
}

} // namespace micro800
