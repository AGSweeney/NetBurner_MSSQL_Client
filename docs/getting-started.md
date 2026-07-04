# Getting started

This guide walks through building the reference firmware, loading it onto a NetBurner module, and running your first SQL query through the web UI.

## Prerequisites

1. **NetBurner NNDK** installed with `NNDK_ROOT` pointing at your SDK tree.
2. **This repository** cloned or copied to a local path (referred to as `NetBurner_MSSQL_Client` below).
3. A **NetBurner module** on the same network as SQL Server (the example targets `NANO54415`).
4. **Microsoft SQL Server** (or compatible TDS endpoint) listening on TCP port **1433**.
5. SQL credentials with permission to connect and query the target database.

## Build the example firmware

```bash
cd examples/NetBurner_MSSQL_Client
make PLATFORM=NANO54415
```

On success, the binary is at:

```
obj/release/NetBurner_MSSQL_Client.bin
```

The first build regenerates `src/htmldata.cpp` from the HTML/CSS assets via NetBurner's `comphtml` tool. This file is gitignored and recreated on every build.

### Other platforms

The example makefile supports:

`NANO54415` · `MOD54415` · `SOMRT1061` · `MODRT1171` · `MODM7AE70`

Replace `PLATFORM=` with your module. After switching between ColdFire and ARM families, run `make clean` before rebuilding.

## Load firmware onto the device

```bash
make PLATFORM=NANO54415 load DEVIP=<device-ip>
```

Alternatively use NetBurner's IDE load workflow with the generated `.bin` or `.s19` file.

## Open the web UI

| URL | Purpose |
|-----|---------|
| `http://<device-ip>/` | Dashboard |
| `http://<device-ip>/configure.html` | Connection setup |
| `http://<device-ip>/browse.html` | Query builder |
| `http://<device-ip>/results.html` | Last result set |
| `http://<device-ip>/console.html` | Live firmware log |

## Configure SQL Server connection

1. Go to **Configure**.
2. Enter **Server** (IP or hostname), **Port** (default 1433), **User**, and **Password**.
3. Use **Refresh Databases** if you want a dropdown of server databases, or type the database name manually.
4. Click **Test Connection**. A successful test verifies login but does not save settings by itself.
5. Click **Save Settings** to persist validated connection details to NV flash (AppData). Saved settings survive reboot.

Leave the password field blank on submit to keep the password already stored in NV flash.

## Run a read query

1. Open **Browse**.
2. Click **Refresh Tables**, then pick a table from the dropdown (or type manually in Browse; on the Write page only the dropdown is available).
3. Click **Refresh Columns** to load column names for filters and the column picker.
4. Choose **Builder** or **Custom SQL**:
   - **Builder** — table, columns, max rows, filter, and sort generate a read-only `SELECT` preview.
   - **Custom SQL** — hides builder controls; edit the SQL textarea directly (must be a `SELECT`).
5. Click **Run Query**.
6. Open **Results** to view the grid. Use **Export CSV** for a download of the last result set.

## Guided writes (optional)

1. Open **Insert / Update**.
2. In **Working Table**, pick a table and click **Load Columns** (tables and columns also refresh automatically when needed).
3. Expand **Guided Insert**, **Guided Update**, or **Custom Write SQL**.
4. Fill fields and click **Preview**. Review the staged SQL in the pending preview panel.
5. Click **Confirm and Execute** or **Cancel**.

Write operations never run without an explicit confirmation step.

## Troubleshooting

| Symptom | Check |
|---------|-------|
| Connection test fails | Route to SQL Server, firewall on 1433, credentials, SQL Server allows TCP |
| Empty table list | Test connection first; click Refresh Tables on Browse or Write |
| No columns | Select a table; click Load Columns on Write or Refresh Columns on Browse |
| Builder/Custom toggle does nothing | Hard-refresh the page; ensure firmware is current (JS must load without errors) |
| Detailed errors | Open `/console.html` for `iprintf` / TDS debug output |

For architecture and internals, see [Architecture](architecture.md) and [Web UI](web-ui.md).
