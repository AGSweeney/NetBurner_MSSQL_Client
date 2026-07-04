# Build system

This guide covers NetBurner makefile integration, generated web assets, and clean build practices for NetBurner_MSSQL_Client.

## Library makefile

`libraries/TDSLite/library.mak` is included by your project when `LIBS_TDSLITE := 1`:

```makefile
TDSLITE_ROOT ?= /path/to/NetBurner_MSSQL_Client
LIBS_TDSLITE  := 1
include $(TDSLITE_ROOT)/libraries/TDSLite/library.mak
```

It only adds include paths — no separate link step.

## Example project makefile

`examples/NetBurner_MSSQL_Client/makefile` defines:

- `PLATFORM` — target module (default `NANO54415`)
- `TDSLITE_ROOT` — relative path to repo root (`../..`)
- Source list including `web.cpp`, `sql_runtime.cpp`, etc.
- HTML/CSS inputs for comphtml
- Standard NetBurner include of `boilerplate.mk`

### Common targets

```bash
make PLATFORM=NANO54415          # Build release binary
make PLATFORM=NANO54415 clean    # Remove obj/ and generated htmldata
make PLATFORM=NANO54415 load DEVIP=x.x.x.x   # Flash device
```

Output binary:

```
obj/release/NetBurner_MSSQL_Client.bin
```

## comphtml and htmldata.cpp

NetBurner compiles HTML into C++ source:

```
html/*.html  +  html/style.css  →  comphtml  →  src/htmldata.cpp
```

- **Do not edit** `htmldata.cpp` manually — changes are overwritten every build
- **Do not commit** `htmldata.cpp` — listed in `.gitignore`
- If web pages look stale after HTML edits, rebuild (or `touch` HTML and rebuild)

CPPCALL symbols in HTML must match registered function names in `web.cpp` exactly.

## Object files and platform switches

Object files under `obj/` are CPU- and optimization-specific. When changing `PLATFORM` across:

- ColdFire (big-endian) ↔ ARM (little-endian)
- Different module families

Always run:

```bash
make clean
make PLATFORM=<new-platform>
```

Mixing objects causes link errors or subtle runtime bugs.

## Gitignored artifacts

Typical entries (see root `.gitignore`):

| Path | Reason |
|------|--------|
| `obj/` | Build output |
| `src/htmldata.cpp` | Generated |
| `*.tmp`, scratch HTML | Local dev files |
| IDE / OS cruft | `.vs/`, `Thumbs.db`, etc. |

A clean clone should build without preexisting `obj/` or `htmldata.cpp`.

## Compiler flags

The example uses NetBurner boilerplate defaults plus C++11 for tdslite:

- `-std=c++11`
- `-fno-exceptions`
- `-fno-rtti`

Optional debug:

```makefile
CFLAGS += -DTDS_DEBUG=1
```

## Header-only compilation cost

Every `.cpp` that includes `TDSLite/tdslite.h` instantiates template code from tdslite. Keep includes in few translation units if code size matters:

- One `.cpp` for all SQL/driver work (as the example does with `sql_runtime.cpp`)
- Web layer includes only runtime headers, not full tdslite

## CI / headless build

Requirements:

1. `NNDK_ROOT` environment variable set
2. Network not required for compile (only for `load`)
3. Run from example directory with explicit `PLATFORM`

Example:

```bash
export NNDK_ROOT=/opt/netburner/ndk
cd examples/NetBurner_MSSQL_Client
make clean
make PLATFORM=NANO54415
```

Verify exit code 0 and binary presence under `obj/release/`.

## Troubleshooting builds

| Error | Likely cause |
|-------|----------------|
| `TDSLite/tdslite.h: No such file` | `TDSLITE_ROOT` wrong or `library.mak` not included |
| comphtml / CPPCALL undefined | HTML references function not implemented in `web.cpp` |
| Duplicate symbol in htmldata | Duplicate HTML route names |
| C++ template errors in tdslite | Missing `-std=c++11` or exceptions enabled |
| Link errors after platform change | Stale `obj/` — run `make clean` |

## Related

- [Getting started](getting-started.md) — first build and flash
- [Web UI](web-ui.md) — comphtml `%` escaping rules
- [Upstream & patches](upstream-and-patches.md) — header compatibility
