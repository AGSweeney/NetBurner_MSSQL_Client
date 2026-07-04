# Example firmware

The reference application in `examples/NetBurner_MSSQL_Client/` demonstrates production-style use of TDSLite: persistent configuration, catalog browsing, safe writes, and a multipage web UI.

## Source files

| File | Responsibility |
|------|----------------|
| `main.cpp` | Boot, HTTP start, remote console, runtime init/poll loop |
| `web.cpp` | CPPCALL renderers, POST handlers, SQL generation, client script injection |
| `sql_runtime.h` / `sql_runtime.cpp` | Job queue, driver lifecycle, query/mutation execution |
| `sql_nv.h` / `sql_nv.cpp` | AppData persistence for connection and query preferences |
| `sql_catalog.h` / `sql_catalog.cpp` | Database/table/column name caches |
| `sql_results.h` / `sql_results.cpp` | Last result storage, CSV export data |
| `html/*.html` | Static page shells with `<!--CPPCALL ...-->` placeholders |
| `html/style.css` | Shared layout and form styling |
| `src/htmldata.cpp` | **Generated** — embedded web assets (do not edit by hand) |

## Configuration model

`sql_runtime_config` (in `sql_runtime.h`) holds:

- Connection: server, port, user, password, database
- Browse context: table name, selected columns (comma-separated)
- Query options: custom SQL text, max rows, filter expression, sort column/direction
- Write context: working table for Insert/Update page

Defaults are applied in `SqlRuntimeInit()`. NV-loaded values override defaults when present.

## Job types

HTTP handlers call `SqlRuntimeRequest*()` functions; the worker task processes one job at a time:

| Request function | Trigger | Behavior |
|------------------|---------|----------|
| `SqlRuntimeRequestTestConnection()` | Configure → Test Connection | Login verify; does not save NV |
| `SqlRuntimeRequestRun()` | Browse → Run Query | Read-only SELECT (builder or custom) |
| `SqlRuntimeRequestBrowseDatabases()` | Configure → Refresh Databases | Fills database catalog |
| `SqlRuntimeRequestBrowseTables()` | Browse/Write → Refresh Tables | Table list for current DB |
| `SqlRuntimeRequestBrowseColumns()` | Browse/Write → Refresh Columns | Column metadata for selected table |
| `SqlRuntimeRequestExecutePendingMutation()` | Write → Confirm | Runs staged INSERT/UPDATE/custom write |

`SqlRuntimePoll()` advances the worker from `main.cpp`'s loop. POST handlers update config, call a `Request*` function, and redirect or re-render the page.

## Connection lifecycle per job

1. Load config (RAM + NV merge)
2. `driver.connect(params)` with current credentials
3. Execute SQL or catalog queries
4. Populate `sql_result_store` or catalogs
5. `driver.disconnect()`
6. Update `sql_connection_status` (idle / busy / last error)

## NV persistence (`sql_nv.cpp`)

Settings live under AppData branch `SqlSettings` via `config_obj`:

- Saved only when user clicks **Save Settings** after successful validation
- Password field blank on Configure form = keep existing stored password
- `SqlNvClear()` removes the branch (factory reset pattern)
- `SqlNvLastVerifiedSecs()` records last successful connection test

Implementation detail: saving must clear the `fConfigHidden` branch flag so NetBurner writes NV flash — see patch notes in `PATCHES.md` if you fork this module.

## Catalogs (`sql_catalog.cpp`)

Catalogs are plain C string arrays with counts:

```cpp
SqlCatalogGetDatabases()  // up to 64 names
SqlCatalogGetTables()
SqlCatalogGetColumns()
```

Invalidation happens when:

- Database name changes
- Table name changes
- Explicit refresh job completes

Catalogs are **not** persisted to NV — they are refreshed from SQL Server on demand.

## Results store (`sql_results.cpp`)

After each query or mutation:

- Column headers and cell text (HTML-escaped at render time in `web.cpp`)
- Row count, duration ms, success flag, user-facing message
- `SqlResultsIsMutation()` distinguishes DML from SELECT for Results page layout
- `SqlResultsExportCsv()` builds RFC-style CSV for `/export.csv`

Maximum rows returned to the module are capped (default 250) regardless of SQL Server result size.

## SQL generation and safety (`web.cpp`)

### Read queries

- **Builder mode** generates `SELECT TOP (N) [cols] FROM [db].[dbo].[table] WHERE ... ORDER BY ...`
- **Custom mode** accepts user SQL but must pass read-only validation (starts with `SELECT`, no forbidden keywords)
- Identifiers are bracket-quoted; filter/sort inputs are validated

### Write queries

Three paths on **Insert / Update**:

1. **Guided Insert** — form fields → parameterized-style INSERT preview
2. **Guided Update** — key column + SET fields → UPDATE with WHERE on key
3. **Custom Write SQL** — user text, validated separately from SELECT rules

All writes go through **staging**:

1. POST builds SQL and stores in pending mutation state
2. User sees preview on the same page (live JS preview + server pending panel)
3. **Confirm and Execute** enqueues mutation job; **Cancel** clears pending state

No write runs on preview POST alone.

### Working table (Write page)

The Write page requires a table selected from the browsed dropdown (`setwritetable` POST). Manual table name entry was removed to avoid typos on DML. **Load Columns** / auto-init refresh column metadata for form fields.

## HTTP integration

- `StartHttp()` in `main.cpp` registers the default NetBurner web server
- POST handlers register at static init time via global `HtmlPostVariableListCallback` objects in `web.cpp` (no separate web init call from `main.cpp`)
- Pages use shared header/footer/nav rendered by CPPCALL helpers (`ShowHeaderStatus`, `ShowQueryStatus`, etc.)

See [Web UI](web-ui.md) for page-by-page behavior and comphtml rules.

## Extending the example

Common extension points:

| Goal | Where to change |
|------|-----------------|
| Add a new page | New `html/foo.html`, CPPCALLs in `web.cpp`, register in makefile HTML list |
| New SQL operation | New job enum in `sql_runtime.h`, handler in `sql_runtime.cpp`, POST in `web.cpp` |
| Different NV layout | `sql_nv.cpp` field names / branch structure |
| Stricter write policy | Validation functions in `web.cpp` before staging |

Keep SQL work off the HTTP thread — always enqueue through `SqlRuntime`.

## Build artifacts

Do not commit:

- `obj/` — object files and binaries
- `src/htmldata.cpp` — regenerated by comphtml

See [Build system](build-system.md).
