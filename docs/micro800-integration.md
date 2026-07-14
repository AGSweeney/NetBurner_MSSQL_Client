# Micro800 client integration

This guide explains how to use the optional **Micro800Client** library in a NetBurner project. The library is an EtherNet/IP + CIP tag client extracted from the working NetBurnerGateway CIP Data Gateway code. **REAJet / print-mapping code is not included.**

The MSSQL example firmware (`examples/NetBurner_MSSQL_Client`) does **not** enable this library by default. Enable it only when your application needs PLC tag access.

## Provenance

| Included | Excluded |
|----------|----------|
| ENIP session (RegisterSession / SendRRData) | REAJet REA-PLC / REA-PI |
| UDP ListIdentity scan | Print mapping runtime |
| CIP Read Tag `0x4C`, Write Tag `0x4D`, List Tags `0x55` | Gateway HTTP `/api/reajet/*`, `/api/mappings/*` |
| Tag path encode + value formatting | Gateway HTML UI / config_obj persistence |

## 1. Add the library to your makefile

Set `MICRO800_ROOT` for include paths, then include `library.mak` **before** `boilerplate.mk`. On Windows, use a **relative** path to `library.mak` so Make does not see drive-letter colons in source rules:

```makefile
MICRO800_ROOT ?= D:/NetBurner_MSSQL_Client
LIBS_MICRO800 := 1
include ../../libraries/Micro800Client/library.mak
```

You can enable both TDSLite and Micro800 in the same project:

```makefile
TDSLITE_ROOT  ?= $(abspath ../..)
LIBS_TDSLITE  := 1
include $(TDSLITE_ROOT)/libraries/TDSLite/library.mak

MICRO800_ROOT ?= $(abspath ../..)
LIBS_MICRO800 := 1
include ../../libraries/Micro800Client/library.mak

include $(NNDK_ROOT)/make/boilerplate.mk
```

`library.mak` adds include path `$(MICRO800_ROOT)/libraries/include` and appends the Micro800 `.cpp` sources to `CPP_SRC` (resolved relative to `library.mak`).

## 2. Include the umbrella header

```cpp
#include <init.h>
#include <Micro800Client/micro800_client.h>
```

## 3. Wait for network before scanning or connecting

```cpp
void UserMain(void *pd)
{
    init();
    WaitForActiveNetwork(TICKS_PER_SECOND * 10);

    // ... scan / probe / read tags ...
}
```

## 4. Scan for EtherNet/IP devices

```cpp
micro800::DiscoveredDevice devices[micro800::kMaxDiscoveredDevices];
char err[96] = {};
const int n = micro800::ScanListIdentity(devices, micro800::kMaxDiscoveredDevices, err, sizeof(err));

for (int i = 0; i < n; ++i) {
    if (micro800::LooksLikeMicro800(devices[i])) {
        iprintf("Micro800 candidate: %hI %s\r\n", devices[i].ip, devices[i].productName);
    }
}
```

Detection heuristic: Rockwell vendor `0x0001` plus product name containing `2080-`, `2085-`, or `micro8*`.

## 5. Probe TCP reachability

```cpp
if (!micro800::ProbePlc(devices[0].ip, err, sizeof(err))) {
    iprintf("Probe failed: %s\r\n", err);
}
```

Opens TCP **44818** and closes immediately (same behavior as the Gateway auto-connect probe).

## 6. Read and write tags

Each call opens a **new** ENIP session, performs one CIP request, UnregisterSession, and closes the socket.

```cpp
micro800::TagValue value{};
if (micro800::ReadTag(plcIp, "MyTag", value, err, sizeof(err))) {
    char text[64] = {};
    micro800::FormatTagValueText(value.typeCode, value.data, value.dataLen, text, sizeof(text));
    iprintf("%s = %s (%s)\r\n",
            "MyTag",
            text,
            micro800::CipTypeName(value.typeCode));
}

uint8_t dintBytes[4] = {1, 0, 0, 0}; // little-endian DINT = 1
micro800::WriteTag(plcIp, "MyTag", 0xC4, dintBytes, sizeof(dintBytes), err, sizeof(err));
```

Raw buffer API:

```cpp
uint16_t typeCode = 0;
uint8_t raw[64] = {};
size_t rawLen = 0;
micro800::ReadTagRaw(plcIp, "MyTag", typeCode, raw, sizeof(raw), rawLen, err, sizeof(err));
```

## 7. Browse PLC tags

```cpp
micro800::BrowsedTag tags[128];
const int count = micro800::BrowseTags(plcIp, tags, 128, err, sizeof(err));
for (int i = 0; i < count; ++i) {
    iprintf("%s type=0x%04X array=%d\r\n",
            tags[i].name,
            tags[i].symbolType,
            tags[i].isArray ? 1 : 0);
}
```

Uses CIP List Tags service `0x55` on Symbol object class `0x6B`.

## 8. Tag path helpers

```cpp
char normalized[micro800::kMaxTagPathLen] = {};
if (micro800::NormalizeTagPath(" MyTag[0].Field ", normalized, sizeof(normalized))) {
    uint8_t path[256] = {};
    size_t pathLen = 0;
    micro800::EncodeCipTagPath(normalized, path, sizeof(path), pathLen);
}
```

Supports dotted paths and array indices such as `Tag.Sub[0].Field`.

## Protocol notes

| Item | Value |
|------|-------|
| ENIP TCP port | **44818** |
| ListIdentity | UDP broadcast; local RX port **44819** |
| RegisterSession | command `0x0065` |
| SendRRData | command `0x006F` |
| CIP Read Tag | service `0x4C` (response `0xCC`) |
| CIP Write Tag | service `0x4D` (response `0xCD`) |
| CIP List Tags | service `0x55` |

**Not supported** (Gateway never implemented these): CompactLogix/ControlLogix-specific browsing models, persistent ENIP sessions, implicit/cyclic I/O, CIP security.

## Optional Micro800 → MSSQL gateway (example firmware)

The `examples/NetBurner_MSSQL_Client` project can optionally build a multi-PLC gateway that
polls Micro800 trigger/ACK tags and inserts/updates MSSQL asynchronously.

| Build | How | Result |
|-------|-----|--------|
| Generic (default) | Leave `LIBS_GATEWAY` unset in the example makefile | Original SQL browser only; no Micro800 or gateway pages |
| Gateway | Uncomment / set `LIBS_GATEWAY := 1` (makefile pulls Micro800Client + `gateway.mak`) | Gateway runtime, HTML pages (`gateway.html`, `plc.html`, `sql_maps.html`), nav links |

Example makefile excerpt:

```makefile
# Optional Micro800→MSSQL gateway. Leave unset for the generic SQL example.
# LIBS_GATEWAY := 1
```

To enable:

```makefile
LIBS_GATEWAY := 1
```

`gateway.mak` defines `NB_GATEWAY_MICRO800`, merges `html/` + `html_gateway/` for `comphtml`, and
adds `src/gateway/*.cpp`.

**Handshake:** level-held trigger/ACK. ACK is asserted in the same capture step after the event is
appended to the gateway queue (before SQL drain). The main loop polls the gateway handshake before
`SqlRuntimePoll` so a blocking INSERT on UserMain cannot delay ACK. SQL drain remains asynchronous
relative to the PLC handshake.

**Event queue:** with `LIBS_GATEWAY=1`, captured events are held in an **in-RAM ring buffer** until
SQL drain completes. ACK is asserted only after `QueueAppend` succeeds. The queue does not survive
reboot or power loss.

Configure PLCs and SQL mappings from the gateway web UI after flashing a gateway build.

## Compiler requirements

- NetBurner NNDK GCC, C++11
- Same platforms as the MSSQL example / ColdFire and ARM NetBurner modules with TCP/UDP

After changing `PLATFORM` between CPU families, run `make clean`.

## What not to copy

| Source | Needed for Micro800 client? |
|--------|-----------------------------|
| NetBurnerGateway `reajet_runtime.cpp` (REAJet portion) | **No** — excluded from this library |
| `mapping_runtime.cpp` | **No** — print workflow only |
| Gateway `http_*` / HTML | **No** — app-layer UI; add your own if desired |
| Gateway `config_obj` PLC persistence | **No** — optional app pattern |
| MSSQL example `sql_runtime` / web UI | **No** — unrelated SQL browser |

Persist selected PLC IP, tag lists, or REST APIs in your own firmware if you need them.

## Next steps

- [Library integration](library-integration.md) — TDSLite (SQL) sibling library
- [Architecture](architecture.md) — repository layout
- [Build system](build-system.md) — makefile / platform notes
