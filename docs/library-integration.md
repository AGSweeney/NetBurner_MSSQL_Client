# Library integration

This guide explains how to use the TDSLite headers in a **new** NetBurner project without copying the example web application.

## 1. Add the library to your makefile

Set `TDSLITE_ROOT` to this repository (or a copy on your build machine), then include the library makefile **before** `boilerplate.mk`:

```makefile
TDSLITE_ROOT ?= D:/NetBurner_MSSQL_Client
LIBS_TDSLITE  := 1
include $(TDSLITE_ROOT)/libraries/TDSLite/library.mak
```

`library.mak` adds these include paths:

```
$(TDSLITE_ROOT)/libraries/include
$(TDSLITE_ROOT)/libraries/include/tdslite
$(TDSLITE_ROOT)/libraries/include/tdslite-net
```

No `.a` or `.o` library file is linked — everything is header-only and compiled into your translation units.

## 2. Include the umbrella header

```cpp
#include <init.h>
#include <TDSLite/tdslite.h>
```

This pulls in:

- Full tdslite stack (`tdslite/tdslite.hpp`)
- Endian self-test (`tdslite/util/tdsl_endian_check.hpp`)
- NetBurner network implementation
- Type alias `tdsl::netburner_driver`

## 3. Allocate a network buffer

The driver reads TDS packets into a caller-supplied buffer:

```cpp
static tdsl::uint8_t net_buf[4096] = {};
static tdsl::netburner_driver driver{net_buf};
```

4096 bytes matches the default packet size used in the example. Do not share this buffer across concurrent driver instances.

## 4. Wait for network before connecting

```cpp
void UserMain(void *pd)
{
    init();
    WaitForActiveNetwork(TICKS_PER_SECOND * 10);

    // ... connect and query ...
}
```

## 5. Connect to SQL Server

Fill `connection_parameters` and call `connect()`:

```cpp
tdsl::netburner_driver::connection_parameters params;
params.server_name = "192.168.1.100";   // IP or hostname
params.port        = 1433;
params.user_name   = "sa";
params.password    = "secret";
params.app_name    = "MyApp";
params.client_name = "NetBurner";
params.db_name     = "MyDatabase";
params.packet_size = 4096;

auto err = driver.connect(params);
if (err != tdsl::netburner_driver::e_driver_error_code::success) {
    iprintf("Connect failed: %d\r\n", static_cast<int>(err));
    return;
}
```

`server_name` is resolved with `GetHostByName`. Use a numeric IP if DNS is unavailable on your network.

## 6. Execute queries

### Simple query (no row handling)

```cpp
driver.execute_query("SELECT 1 AS x;", nullptr, nullptr);
```

### Query with row callback

```cpp
struct ctx {
    tdsl::uint32_t row_count;
};

auto on_row = [](void * user, const tdsl::detail::tdsl_data_reader & reader) {
    auto * c = static_cast<ctx *>(user);
    ++c->row_count;
    // Access columns via reader.get_field(index).as<T>()
};

ctx c{};
driver.execute_query("SELECT TOP (10) name FROM sys.tables;", on_row, &c);
iprintf("Rows: %lu\r\n", c.row_count);
```

Refer to upstream tdslite documentation for RPC, parameterized queries, and field type details.

## 7. Disconnect

```cpp
driver.disconnect();
```

Always disconnect when finished. The example runtime connects per job and disconnects in a `finally`-style path.

## 8. Debugging

Enable tdslite debug output in your makefile or source:

```cpp
#define TDS_DEBUG 1
```

Before including tdslite headers, or add `-DTDS_DEBUG=1` to `CFLAGS`.

Use `iprintf` and the remote console (`EnableRemoteConsole()` + `/console.html`) to view logs without a serial cable.

## Compiler requirements

The vendored tdslite is patched for NetBurner GCC:

- C++11 (`-std=c++11`)
- No exceptions (`-fno-exceptions`)
- No RTTI (`-fno-rtti`)

Do not enable exceptions in translation units that include tdslite headers unless you re-validate upstream compatibility.

See [Upstream & patches](upstream-and-patches.md) for the full patch list.

## What not to copy from the example

| Example component | Needed for bare TDS? |
|-------------------|----------------------|
| `sql_runtime.cpp` | No — optional job queue pattern |
| `web.cpp` / HTML | No — UI only |
| `sql_nv.cpp` | No — optional persistence |
| `tdsl_netimpl_netburner.hpp` | Already in library |

Copy patterns from the example only when you need similar behavior (NV config, web forms, catalog caching).

## Platform notes

After changing `PLATFORM` between ColdFire and ARM, run `make clean`. Object files from different endian/ABI targets must not be mixed.

Test on your target hardware early — endian conversion is compile-time verified, but SQL Server authentication and network timing vary by deployment.

## Next steps

- [Architecture](architecture.md) — how layers fit together
- [Example firmware](example-firmware.md) — if you want the full SQL browser as a starting point
- [Build system](build-system.md) — comphtml and makefile details for multipage projects
