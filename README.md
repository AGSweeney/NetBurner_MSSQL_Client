# NetBurner_MSSQL_Client

**Microsoft SQL Server connectivity for NetBurner modules** — a header-only TDS (Tabular Data Stream) client library plus a reference firmware that exposes a full SQL browser over the device web UI.

This repository lets embedded NetBurner applications talk to SQL Server without ODBC, without Windows, and without a large dependency stack. The protocol engine comes from the upstream [tdslite](https://github.com/tdslite/tdslite) project; this repo adds the NetBurner TCP adapter, toolchain compatibility patches, and a production-style example application.

---

## What is in this repository?

| Area | Path | Purpose |
|------|------|---------|
| **TDS library (header-only)** | `libraries/include/tdslite/` | MS-TDS protocol stack (vendored, patched) |
| **Network abstraction** | `libraries/include/tdslite-net/` | Generic I/O base + NetBurner TCP implementation |
| **Umbrella header** | `libraries/include/TDSLite/tdslite.h` | Single include for firmware projects |
| **Make integration** | `libraries/TDSLite/library.mak` | Adds include paths to your NetBurner makefile |
| **Reference firmware** | `examples/NetBurner_MSSQL_Client/` | Web-based SQL browser, query runner, guided writes |
| **Patch log** | [`PATCHES.md`](PATCHES.md) | Changes applied to upstream tdslite for NetBurner GCC |
| **Deep docs** | [`docs/`](docs/) | Architecture, integration, web UI, and build guides |

---

## What can the example firmware do?

The **NetBurner MSSQL Client** (`examples/NetBurner_MSSQL_Client`) is a multipage web application served directly from the module:

| Page | Function |
|------|----------|
| **Dashboard** | Entry point and navigation |
| **Configure** | Server, port, user, password, database; test connection; save to NV flash |
| **Browse** | Table/column catalog, query builder or custom `SELECT`, run read queries |
| **Insert / Update** | Pick a working table, guided insert/update, or custom write SQL with confirmation |
| **Results** | Scrollable result grid, CSV export |
| **Diagnostics** | Connection state, query mode, timing, link to remote console |

Additional endpoints:

- **`/console.html`** — live `printf()` output (no serial cable required)
- **`/export.csv`** — download last query results

Connection settings persist in NetBurner **AppData** (`config_obj`) across reboots. Write operations are staged, previewed, and require explicit confirmation before execution.

---

## Quick start

### Prerequisites

- NetBurner NNDK installed (`NNDK_ROOT` set)
- A supported module (example targets **NANO54415**; library supports ColdFire and ARM platforms listed in the example makefile)
- Network reachability to SQL Server on TCP **1433**

### Build and load the example

```bash
cd examples/NetBurner_MSSQL_Client
make PLATFORM=NANO54415
make PLATFORM=NANO54415 load DEVIP=<device-ip>
```

### Use the web UI

1. Open `http://<device-ip>/`
2. **Configure** — enter SQL Server details, click **Test Connection**, then **Save Settings**
3. **Browse** — refresh tables/columns, build or type a `SELECT`, **Run Query**
4. **Results** — review data; export CSV if needed
5. **Insert / Update** (optional) — choose working table, load columns, preview and confirm writes

Remote console: `http://<device-ip>/console.html`

Detailed walkthrough: [`docs/getting-started.md`](docs/getting-started.md)

---

## Use the library in your own project

Add to your makefile **before** `boilerplate.mk`:

```makefile
TDSLITE_ROOT ?= /path/to/NetBurner_MSSQL_Client
LIBS_TDSLITE  := 1
include $(TDSLITE_ROOT)/libraries/TDSLite/library.mak
```

Minimal application code:

```cpp
#include <init.h>
#include <TDSLite/tdslite.h>

static tdsl::uint8_t net_buf[4096] = {};
static tdsl::netburner_driver driver{net_buf};

void UserMain(void *pd)
{
    init();
    WaitForActiveNetwork(TICKS_PER_SECOND * 10);

    tdsl::netburner_driver::connection_parameters params;
    params.server_name = "192.168.1.100";
    params.port        = 1433;
    params.user_name   = "sa";
    params.password    = "your_password";
    params.app_name    = "MyNetBurnerApp";
    params.client_name = "NetBurner";
    params.db_name     = "MyDatabase";
    params.packet_size = 4096;

    if (driver.connect(params) != tdsl::netburner_driver::e_driver_error_code::success) {
        iprintf("SQL connect failed\r\n");
        return;
    }

    driver.execute_query("SELECT TOP (10) * FROM [MyDatabase].[dbo].[MyTable];",
                         nullptr, nullptr);
    driver.disconnect();
}
```

Full integration guide: [`docs/library-integration.md`](docs/library-integration.md)

---

## Architecture at a glance

```
┌─────────────────────────────────────────────────────────┐
│  Your firmware (or NetBurner_MSSQL_Client example)      │
│  web.cpp · sql_runtime.cpp · main.cpp                   │
└──────────────────────────┬──────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────┐
│  tdsl::netburner_driver  (TDSLite/tdslite.h)            │
│  TCP via connect / ReadAllWithTimeout / writeall        │
└──────────────────────────┬──────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────┐
│  tdslite header-only stack (driver, login, tokens, rows)  │
└──────────────────────────┬──────────────────────────────┘
                           │ MS-TDS / TCP 1433
┌──────────────────────────▼──────────────────────────────┐
│  Microsoft SQL Server                                     │
└───────────────────────────────────────────────────────────┘
```

Endianness: TDS wire format is little-endian. The library detects host byte order at compile time and converts on big-endian ColdFire (NANO54415, MOD54415) and little-endian ARM targets alike.

Deep dive: [`docs/architecture.md`](docs/architecture.md)

---

## Supported platforms

The example makefile lists:

`NANO54415` · `MOD54415` · `SOMRT1061` · `MODRT1171` · `MODM7AE70`

When switching `PLATFORM` across CPU/endian families, run `make clean` first — object files are not interchangeable.

---

## Upstream tdslite

Core headers are vendored from **[tdslite v0.8.0](https://github.com/tdslite/tdslite/releases/tag/v0.8.0)** (see [`UPSTREAM_VERSION`](UPSTREAM_VERSION)).

NetBurner-specific changes are documented in [`PATCHES.md`](PATCHES.md) and summarized in [`docs/upstream-and-patches.md`](docs/upstream-and-patches.md).

---

## Documentation index

| Guide | Description |
|-------|-------------|
| [Getting started](docs/getting-started.md) | Build, flash, configure SQL Server, first query |
| [Architecture](docs/architecture.md) | Library layers, example app modules, data flow |
| [Library integration](docs/library-integration.md) | Add TDSLite to a new NetBurner project |
| [Example firmware](docs/example-firmware.md) | SQL runtime, NV storage, catalogs, job queue |
| [Web UI](docs/web-ui.md) | Pages, CPPCALL handlers, query/write workflows |
| [Build system](docs/build-system.md) | Make, comphtml, generated files, clean builds |
| [Upstream & patches](docs/upstream-and-patches.md) | Vendoring policy and patch maintenance |

---

## License

- **First-party** code (NetBurner adapter, example firmware, docs): MIT — Copyright 2026 Adam G. Sweeney
- **Vendored tdslite** headers: MIT — Copyright 2022 mkg
- **NetBurner SDK** generated material retains NetBurner notices

See [`LICENSE.md`](LICENSE.md) for full details.
