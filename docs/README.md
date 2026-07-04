# NetBurner_MSSQL_Client documentation

This folder contains implementation and usage guides for the NetBurner_MSSQL_Client repository.

## Guides

| Document | Audience | Contents |
|----------|----------|----------|
| [Getting started](getting-started.md) | New users | Build, flash, configure, first query |
| [Architecture](architecture.md) | Developers | System design, modules, threading model |
| [Library integration](library-integration.md) | Firmware authors | Add TDSLite to your own NetBurner project |
| [Example firmware](example-firmware.md) | Contributors | Reference app internals (`sql_runtime`, NV, catalogs) |
| [Web UI](web-ui.md) | UI / backend work | Multipage app, CPPCALL, forms, safety rules |
| [Build system](build-system.md) | Build / CI | Make, comphtml, artifacts, platform switches |
| [Upstream & patches](upstream-and-patches.md) | Maintainers | Vendoring tdslite, re-applying patches |

## Related files at repository root

- [`README.md`](../README.md) — project overview
- [`PATCHES.md`](../PATCHES.md) — line-by-line patch reference
- [`LICENSE.md`](../LICENSE.md) — licensing
- [`UPSTREAM_VERSION`](../UPSTREAM_VERSION) — pinned tdslite release tag
