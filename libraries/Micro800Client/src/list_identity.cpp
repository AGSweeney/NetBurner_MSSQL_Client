// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney
//
// UDP ListIdentity scan — from NetBurnerGateway enip_runtime.cpp.

#include <micro800/list_identity.hpp>
#include <micro800/endian.hpp>
#include <micro800/util.hpp>

#include <cctype>
#include <cstring>

#include <iosys.h>
#include <nbrtos.h>
#include <netinterface.h>
#include <nettypes.h>
#include <stdio.h>
#include <udp.h>

namespace micro800 {

bool LooksLikeMicro800(uint16_t vendorId, const char *productName)
{
    // Rockwell Automation vendor ID is 0x0001.
    if (vendorId != 0x0001) {
        return false;
    }

    // Micro800 families commonly use 2080/2085 catalog prefixes.
    if (ContainsNoCase(productName, "2080-") || ContainsNoCase(productName, "2085-")) {
        return true;
    }

    return ContainsNoCase(productName, "micro8") || ContainsNoCase(productName, "micro800");
}

bool LooksLikeMicro800(const DiscoveredDevice &device)
{
    return device.micro800 || LooksLikeMicro800(device.vendorId, device.productName);
}

static int BuildBroadcastTargets(IPADDR4 *targets, int maxTargets)
{
    if (!targets || maxTargets <= 0) {
        return 0;
    }

    int count = 0;
    int ifNumber = GetFirstInterface();
    while (ifNumber && count < maxTargets) {
        const IPADDR4 ip = InterfaceIP(ifNumber);
        const IPADDR4 mask = InterfaceMASK(ifNumber);
        const uint32_t ipRaw = static_cast<uint32_t>(ip);
        const uint32_t maskRaw = static_cast<uint32_t>(mask);
        if (ipRaw != 0 && maskRaw != 0) {
            const uint32_t bcastRaw = (ipRaw & maskRaw) | (~maskRaw);
            const IPADDR4 candidate = IPV4FromConst(bcastRaw);

            bool duplicate = false;
            for (int i = 0; i < count; ++i) {
                if (targets[i] == candidate) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                targets[count++] = candidate;
            }
        }
        ifNumber = GetNextInterface(ifNumber);
    }

    const IPADDR4 fallback = IPADDR4::GlobalBroadCast();
    bool hasFallback = false;
    for (int i = 0; i < count; ++i) {
        if (targets[i] == fallback) {
            hasFallback = true;
            break;
        }
    }
    if (!hasFallback && count < maxTargets) {
        targets[count++] = fallback;
    }

    return count;
}

static void SanitizeProductName(char *text)
{
    if (!text) {
        return;
    }

    for (size_t i = 0; text[i] != '\0'; ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' ||
            c == '_' || c == '-' || c == '.' || c == '/') {
            continue;
        }
        text[i] = '_';
    }
}

int ScanListIdentity(DiscoveredDevice *out, size_t capacity, char *errorOut, size_t errorOutSize)
{
    static const uint16_t kLocalScanPort = 44819;
    static const uint16_t kCmdListIdentity = 0x0063;
    static const uint16_t kIdentityItemType = 0x000C;
    static const uint32_t kScanWindowTicks = TICKS_PER_SECOND * 3;
    static const uint32_t kSelectSliceTicks = TICKS_PER_SECOND;
    static const uint32_t kResendTicks = TICKS_PER_SECOND;
    static const uint32_t kQuietTicks = TICKS_PER_SECOND / 2;
    static const int kMaxTargets = 8;

    SetError(errorOut, errorOutSize, "");
    if (!out || capacity == 0) {
        SetError(errorOut, errorOutSize, "No output buffer");
        return 0;
    }
    if (capacity > static_cast<size_t>(kMaxDiscoveredDevices)) {
        capacity = static_cast<size_t>(kMaxDiscoveredDevices);
    }
    for (size_t i = 0; i < capacity; ++i) {
        out[i] = DiscoveredDevice{};
    }

    const int firstIf = GetFirstInterface();
    const IPADDR4 sendAnchorIp = (firstIf > 0) ? InterfaceIP(firstIf) : IPADDR4::GlobalBroadCast();
    const int rxSock = CreateRxUdpSocket(kLocalScanPort);
    if (rxSock < 0) {
        SetError(errorOut, errorOutSize, "Failed to create RX UDP socket");
        return 0;
    }

    const int txSock = CreateTxUdpSocket4(sendAnchorIp, kEnipPort, kLocalScanPort);
    if (txSock < 0) {
        SetError(errorOut, errorOutSize, "Failed to create TX UDP socket");
        close(rxSock);
        return 0;
    }

    uint8_t request[24] = {0};
    request[0] = static_cast<uint8_t>(kCmdListIdentity & 0xFF);
    request[1] = static_cast<uint8_t>((kCmdListIdentity >> 8) & 0xFF);

    IPADDR4 targets[kMaxTargets];
    const int targetCount = BuildBroadcastTargets(targets, kMaxTargets);
    if (targetCount <= 0) {
        SetError(errorOut, errorOutSize, "No broadcast targets available");
        close(txSock);
        close(rxSock);
        return 0;
    }

    auto sendRequests = [&]() -> bool {
        bool anySent = false;
        for (int i = 0; i < targetCount; ++i) {
            const int sent = sendto4(txSock, request, sizeof(request), targets[i], kEnipPort);
            if (sent == static_cast<int>(sizeof(request))) {
                anySent = true;
            }
        }
        return anySent;
    };

    if (!sendRequests()) {
        SetError(errorOut, errorOutSize, "ListIdentity send failed");
        close(txSock);
        close(rxSock);
        return 0;
    }

    IPADDR4 foundIps[kMaxDiscoveredDevices];
    size_t foundCount = 0;
    const uint32_t scanStart = TimeTick;
    uint32_t lastSendTick = scanStart;
    uint32_t lastResponseTick = 0;

    while ((TimeTick - scanStart) < kScanWindowTicks && foundCount < capacity) {
        const uint32_t now = TimeTick;
        if ((now - lastSendTick) >= kResendTicks) {
            sendRequests();
            lastSendTick = now;
        }
        if (lastResponseTick != 0 && (now - lastResponseTick) >= kQuietTicks) {
            break;
        }

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(rxSock, &readSet);

        const uint32_t elapsed = now - scanStart;
        const uint32_t remaining = (elapsed < kScanWindowTicks) ? (kScanWindowTicks - elapsed) : 0;
        const uint32_t waitTicks = (remaining < kSelectSliceTicks) ? remaining : kSelectSliceTicks;
        if (waitTicks == 0) {
            break;
        }

        const int ready = select(rxSock + 1, &readSet, nullptr, nullptr, waitTicks);
        if (ready <= 0) {
            continue;
        }

        uint8_t buffer[512] = {0};
        IPADDR4 fromIp{};
        uint16_t localPort = 0;
        uint16_t remotePort = 0;
        const int received = recvfrom4(rxSock, buffer, sizeof(buffer), &fromIp, &localPort, &remotePort);
        if (received < 30) {
            continue;
        }
        lastResponseTick = TimeTick;

        const uint16_t cmd = ReadLe16(buffer + 0);
        if (cmd != kCmdListIdentity) {
            continue;
        }

        const uint16_t payloadLen = ReadLe16(buffer + 2);
        if (payloadLen == 0) {
            continue;
        }

        const uint16_t itemCount = ReadLe16(buffer + 24);
        if (itemCount == 0) {
            continue;
        }

        size_t offset = 26; // 24-byte ENIP header + 2-byte item count
        if (offset + 4 > static_cast<size_t>(received)) {
            continue;
        }

        const uint16_t itemType = ReadLe16(buffer + offset);
        offset += 2;
        const uint16_t itemLength = ReadLe16(buffer + offset);
        offset += 2;
        if (itemType != kIdentityItemType || offset + itemLength > static_cast<size_t>(received)) {
            continue;
        }

        bool duplicate = false;
        for (size_t i = 0; i < foundCount; ++i) {
            if (foundIps[i] == fromIp) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        uint16_t vendorId = 0;
        uint16_t deviceType = 0;
        uint16_t productCode = 0;
        uint8_t majorRev = 0;
        uint8_t minorRev = 0;
        uint32_t serial = 0;
        char productName[33] = {0};

        const uint8_t *itemData = buffer + offset;
        if (itemLength >= 0x18) {
            vendorId = ReadLe16(itemData + 0x12);
            deviceType = ReadLe16(itemData + 0x14);
            productCode = ReadLe16(itemData + 0x16);
        }
        if (itemLength >= 0x20) {
            majorRev = itemData[0x18];
            minorRev = itemData[0x19];
            serial = ReadLe32(itemData + 0x1C);
        }
        if (itemLength > 0x20) {
            const uint8_t nameLen = itemData[0x20];
            const size_t availableName = itemLength - 0x21;
            const size_t copyLen = (nameLen < availableName) ? nameLen : availableName;
            const size_t safeLen = (copyLen < sizeof(productName) - 1) ? copyLen : (sizeof(productName) - 1);
            memcpy(productName, itemData + 0x21, safeLen);
            productName[safeLen] = '\0';
        }

        foundIps[foundCount] = fromIp;
        const bool micro800 = LooksLikeMicro800(vendorId, productName);
        SanitizeProductName(productName);

        DiscoveredDevice &device = out[foundCount];
        device.ip = fromIp;
        device.vendorId = vendorId;
        device.deviceType = deviceType;
        device.productCode = productCode;
        device.majorRev = majorRev;
        device.minorRev = minorRev;
        device.serial = serial;
        const size_t nameLen = strlen(productName);
        const size_t copyLen =
            (nameLen < (sizeof(device.productName) - 1)) ? nameLen : (sizeof(device.productName) - 1);
        memcpy(device.productName, productName, copyLen);
        device.productName[copyLen] = '\0';
        device.micro800 = micro800;
        device.valid = true;
        ++foundCount;
    }

    close(txSock);
    close(rxSock);
    return static_cast<int>(foundCount);
}

} // namespace micro800
