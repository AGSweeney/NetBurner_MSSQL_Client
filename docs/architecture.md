# Architecture

NetBurner_MSSQL_Client separates the **TDS protocol engine** from **platform networking** and, in the example firmware, from **web UI and job scheduling**.

## Repository layout

```
NetBurner_MSSQL_Client/
├── libraries/
│   ├── include/
│   │   ├── tdslite/              # Vendored protocol stack (header-only)
│   │   ├── tdslite-net/
│   │   │   ├── base/             # network_io_base, contract
│   │   │   └── netburner/        # NetBurner TCP adapter
│   │   └── TDSLite/
│   │       └── tdslite.h         # Umbrella include + netburner_driver alias
│   └── TDSLite/
│       └── library.mak           # NBINCLUDE paths for your makefile
├── examples/
│   └── NetBurner_MSSQL_Client/   # Reference SQL browser firmware
│       ├── html/                 # Web assets (comphtml input)
│       └── src/                  # C++ application logic
├── PATCHES.md                    # Upstream compatibility changes
└── docs/                         # This documentation
```

## TDS library stack

### Layer 1 — tdslite core (`libraries/include/tdslite/`)

Pure C++11 implementation of MS-TDS:

- Login (PRELOGIN + LOGIN7)
- Token stream parsing (column metadata, rows, DONE, errors)
- Query and RPC execution
- Row callbacks with typed field access

No sockets, no RTOS — only protocol logic and a pluggable network contract.

### Layer 2 — Network I/O base (`libraries/include/tdslite-net/base/`)

`network_io_base` implements TDS framing (read header, read body, dispatch tokens) and calls into a platform-specific implementation for TCP send/receive.

### Layer 3 — NetBurner adapter (`tdsl_netimpl_netburner.hpp`)

Implements the contract using NetBurner APIs:

| Operation | NetBurner API |
|-----------|---------------|
| Connect | `GetHostByName`, `connect` |
| Send | `writeall` (header + body coalesced when possible) |
| Recv | `SockReadWithTimeout` loop for exact byte counts |
| Disconnect | `close` |

The public type alias is:

```cpp
namespace tdsl { using netburner_driver = driver<net::tdsl_netimpl_netburner>; }
```

Include via `#include <TDSLite/tdslite.h>`.

### Endianness

TDS on the wire is **little-endian**. NetBurner modules span:

| CPU family | Endian | Example platforms |
|------------|--------|-------------------|
| ColdFire | Big-endian | NANO54415, MOD54415 |
| ARM Cortex | Little-endian | SOMRT1061, MODRT1171 |

The library uses `__BYTE_ORDER__` detection, `native_to_le()` / `le_to_native()` helpers, and a compile-time self-test included from `tdslite.h`. Always decode result fields with `field.as<T>()` — never cast raw bytes on big-endian hosts.

## Example firmware architecture

The reference application (`NetBurner_MSSQL_Client`) adds layers above the driver:

```
┌──────────────────────────────────────────────────────────┐
│  HTML pages (comphtml → htmldata.cpp)                    │
│  CPPCALL handlers in web.cpp render dynamic content      │
└────────────────────────┬─────────────────────────────────┘
                         │ HTTP POST / form actions
┌────────────────────────▼─────────────────────────────────┐
│  web.cpp — form parsing, SQL generation, safety checks   │
└────────────────────────┬─────────────────────────────────┘
                         │
┌────────────────────────▼─────────────────────────────────┐
│  sql_runtime.cpp — single-threaded job queue             │
│  connect · query · browse catalogs · staged mutations    │
└─────┬──────────────┬──────────────┬────────────────────────┘
      │              │              │
 sql_nv.cpp    sql_catalog.cpp  sql_results.cpp
 (AppData)     (RAM catalogs)    (last result store)
      │              │              │
      └──────────────┴──────────────┘
                         │
              tdsl::netburner_driver
                         │
                   SQL Server :1433
```

### Main loop (`main.cpp`)

1. `init()`, `SqlNvInit()`, `SqlRuntimeInitDefaults()`, `StartHttp()`, wait for network
2. `EnableRemoteConsole()` for web-based logging
3. `SqlRuntimeInit()` starts the SQL worker task
4. Forever: `SqlRuntimePoll()` + short delay

HTTP runs on the NetBurner web server thread; SQL work runs in a dedicated RTOS task so the UI stays responsive.

### SQL runtime (`sql_runtime.cpp`)

Central coordinator:

- Holds current `sql_runtime_config` (server, database, table, query options)
- Queues jobs: test connection, run query, browse databases/tables/columns, execute confirmed mutation
- Owns one `netburner_driver` instance and connects per job
- Updates `sql_connection_status` and `sql_result_store`

Only one SQL job runs at a time (`SqlRuntimeIsBusy()`). Pages auto-refresh while a job is active.

### NV storage (`sql_nv.cpp`)

Uses NetBurner `config_obj` under an `SqlSettings` branch in AppData:

- Server, port, user, password, database
- Table, columns, query builder options (filter, sort, top N)
- Last verified timestamp

Settings are saved explicitly via **Save Settings** on the Configure page after a successful test.

### Catalogs (`sql_catalog.cpp`)

In-RAM caches populated from `INFORMATION_SCHEMA` / `sys.databases`:

| Catalog | Max entries | Purpose |
|---------|-------------|---------|
| Databases | 64 | Configure page picker |
| Tables | 64 | Browse / Write table dropdown |
| Columns | 64 | Builder, filters, guided writes |

Catalogs invalidate when database or table changes.

### Results (`sql_results.cpp`)

Stores the last query or mutation outcome:

- Row/column text for HTML table rendering
- Success flag, status message, duration in ms
- CSV export source
- Mutation vs SELECT distinction

### Web layer (`web.cpp`)

- **CPPCALL** functions embed dynamic HTML fragments at serve time
- **HtmlPostVariableListCallback** handlers process form POSTs (`runquery`, `testconnection`, `setwritetable`, etc.)
- Client-side script in `ShowFormScript` drives query builder preview and write-page live SQL preview
- Safety: read-only guard on custom SELECTs; write SQL must be staged and confirmed; identifier validation for guided DML

## Design constraints

| Constraint | Rationale |
|------------|-----------|
| Single SQL connection per job | tdslite driver is not concurrent; simplifies buffer ownership |
| ~4 KB network buffer | Matches tdslite defaults; wide result sets stream via callbacks |
| 768-byte max custom query in config | Fits NV field sizes; builder generates within same limit |
| 250 row hard cap | Protects module RAM on large tables |
| No exceptions / RTTI | NetBurner GCC flags; patched upstream templates accordingly |

## Further reading

- [Example firmware internals](example-firmware.md)
- [Web UI workflows](web-ui.md)
- [Library integration](library-integration.md)
