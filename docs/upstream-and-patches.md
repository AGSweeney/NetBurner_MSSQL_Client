# Upstream tdslite and patches

NetBurner_MSSQL_Client vendors the [tdslite](https://github.com/tdslite/tdslite) header-only TDS client and adapts it for NetBurner's embedded GCC toolchain. This document explains versioning policy and how to refresh upstream.

## Pinned version

The file [`UPSTREAM_VERSION`](../UPSTREAM_VERSION) at the repository root records the vendored release tag (currently **v0.8.0**).

When reporting bugs, distinguish:

- **Upstream tdslite behavior** — protocol, token parsing, API design
- **NetBurner patches** — compiler compatibility, endian helpers
- **First-party code** — NetBurner adapter, example firmware, docs

## Why patches exist

NetBurner firmware builds typically use:

- GCC with **no C++ exceptions**
- **No RTTI**
- **C++11** (not C++17)
- Mixed **big-endian** (ColdFire) and **little-endian** (ARM) targets

Upstream tdslite targets general-purpose C++17-ish environments. Patches make the headers compile and behave correctly on NetBurner modules.

## Patch categories

Full line-by-line reference: [`PATCHES.md`](../PATCHES.md)

Summary:

| Category | Examples |
|----------|----------|
| Exception removal | Replace `throw` paths with error codes / assertions |
| Template / SFINAE | C++11-compatible type traits |
| Endian | `native_to_le`, `le_to_native`, field decode helpers |
| NetBurner types | `uint32_t` width, string handling |
| Debug | Optional `TDS_DEBUG` logging hooks |

## NetBurner-specific additions (not upstream)

These live outside the vendored tree or in clearly marked first-party headers:

| Component | Location |
|-----------|----------|
| TCP adapter | `libraries/include/tdslite-net/netburner/tdsl_netimpl_netburner.hpp` |
| Network base | `libraries/include/tdslite-net/base/` |
| Umbrella include | `libraries/include/TDSLite/tdslite.h` |
| Endian self-test | Included from umbrella header |

## Updating upstream tdslite

Recommended process:

1. **Tag diff** — compare new upstream release to current `UPSTREAM_VERSION`
2. **Copy headers** — replace `libraries/include/tdslite/` from upstream tarball/tag
3. **Re-apply patches** — walk [`PATCHES.md`](../PATCHES.md) section by section
4. **Update** `UPSTREAM_VERSION` to new tag
5. **Build all platforms** — at minimum one ColdFire and one ARM target:

   ```bash
   make clean && make PLATFORM=NANO54415
   make clean && make PLATFORM=SOMRT1061
   ```

6. **Runtime test** — connection, SELECT with numeric/string columns, INSERT/UPDATE on example firmware
7. **Document** — add new patch sections to `PATCHES.md` if upstream introduced incompatible code

Do not assume a clean upstream drop will compile without re-reading `PATCHES.md`.

## Licensing

- Vendored tdslite: **MIT** (Copyright 2022 mkg) — retain upstream license headers in copied files
- NetBurner adapter and example: **MIT** (Copyright 2026 Adam G. Sweeney)
- See [`LICENSE.md`](../LICENSE.md)

## Contributing patches upstream

Fixes that are not NetBurner-specific (true protocol bugs, portable C++11 improvements) may be suitable for contribution to [tdslite/tdslite](https://github.com/tdslite/tdslite). Keep a local patch entry until an upstream release contains the fix, then drop the redundant patch during the next vendor refresh.

## Endian verification

The umbrella header includes a compile-time endian check. After any upstream merge affecting integer serialization:

1. Build for NANO54415 (big-endian)
2. Query mixed-type columns (INT, BIGINT, FLOAT, NVARCHAR)
3. Compare values to SQL Server Management Studio or `sqlcmd`

Wrong endian handling shows as garbage numbers or shifted strings — not always as hard connection failures.
