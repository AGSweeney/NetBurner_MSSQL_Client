# Motoman HSES client integration

This guide explains how to use the optional **HsesClient** library — a **NetBurner port of MotoHSES** (`moto_hses.h` / `moto_hses.cpp`): Yaskawa Motoman **High-Speed Ethernet Server (HSES)** over UDP **10040**.

This is **not** Motoman EtherNet/IP CIP (TCP 44818). The implementation is derived from the author's MotoHSES sources (MIT), with NetBurner UDP sockets and fixed buffers instead of host STL/`Winsock`.

The MSSQL example firmware does **not** enable this library by default.

## Provenance

| Included | Excluded / deferred |
|----------|---------------------|
| `moto::hses::Client` request framing + helpers | File-control streaming (UDP **10041**) |
| Status, position, vars, I/O, registers | — |
| Servo / hold / job / typed move helpers | — |
| NetBurner `CreateRxUdpSocket` transport | Host Winsock / BSD sockets |

## 1. Add the library to your makefile

On Windows, use a **relative** path to `library.mak` so Make does not see drive-letter colons in source rules:

```makefile
HSES_ROOT ?= $(abspath ../..)
LIBS_HSES := 1
include ../../libraries/HsesClient/library.mak
```

## 2. Include the umbrella header

```cpp
#include <init.h>
#include <HsesClient/hses_client.h>
```

## 3. Wait for network, then open a client

```cpp
void UserMain(void *pd)
{
    init();
    WaitForActiveNetwork(TICKS_PER_SECOND * 10);

    moto::hses::Endpoint ep;
    ep.ip = IPV4FromConst((192u << 24) | (168u << 16) | (0u << 8) | 1u);
    ep.port = moto::hses::kDefaultControlPort;

    moto::hses::Client client;
    char err[96] = {};
    if (!client.open(ep, moto::hses::kDefaultLocalUdpPort, err, sizeof(err))) {
        iprintf("HSES open failed: %s\r\n", err);
        return;
    }

    moto::hses::Response st = client.readStatusAll();
    if (st.ok()) {
        moto::hses::StatusFlags flags = moto::hses::Client::decodeStatus(st);
        iprintf("teach=%d play=%d remote=%d servo=%d\r\n",
                flags.teach_mode, flags.play_mode, flags.command_remote, flags.servo_on);
    }

    moto::hses::Response pos = client.readPositionPulse();
    moto::hses::PositionReadData axes{};
    if (pos.ok() && moto::hses::decode::positionRead(pos.payload, pos.payload_len, axes)) {
        iprintf("J1=%ld\r\n", static_cast<long>(axes.axis_data[0]));
    }

    client.writeByteVariable(10, 1);
    client.writeOutput(10010, 1);
    client.close();
}
```

## Protocol notes

| Item | Value |
|------|-------|
| Control port | UDP **10040** |
| File port | UDP **10041** — not streamed in this port |
| Magic | `YERC` |
| Header size | 32 bytes |
| Variable R/W | `Get`/`SetAttributeSingle` (`0x0E` / `0x10`) |
| I/O write | `GetAttributeSingle` (`0x0E`) + payload (DX200 quirk, same as MotoHSES) |

Controller side typically needs High-Speed Ethernet Server enabled and appropriate REMOTE / host-communication settings. Firewall must allow UDP 10040 between the NetBurner and the robot.

## Compiler requirements

- NetBurner NNDK GCC, C++11
- After changing `PLATFORM` between CPU families, run `make clean`

## What not to copy

| Source | Needed for HSES client? |
|--------|-------------------------|
| MotoPlus on-controller apps | **No** — different target |
| CDG `enip_scanner_motoman.c` | **No** — Motoman CIP over ENIP |
| Host MotoHSES as-is | **No** — use this NetBurner port |
| Micro800Client | Separate optional ENIP library |

## Next steps

- [Micro800 integration](micro800-integration.md)
- [Library integration](library-integration.md)
- [Build system](build-system.md)

See also [libraries/HsesClient/THIRD_PARTY_NOTICES.txt](../libraries/HsesClient/THIRD_PARTY_NOTICES.txt).
