# Web UI

The NetBurner MSSQL Client is a **multipage** NetBurner web application. Each page is a separate HTML file compiled into firmware via `comphtml`.

## Page map

| File | Route | Role |
|------|-------|------|
| `index.html` | `/` | Dashboard cards linking to workflows |
| `configure.html` | `/configure.html` | Connection settings, test, save |
| `browse.html` | `/browse.html` | Catalog refresh, query builder, custom SELECT |
| `write.html` | `/write.html` | Guided insert/update, custom write, confirmation |
| `results.html` | `/results.html` | Result grid, export link |
| `diagnostics.html` | `/diagnostics.html` | Runtime status, debug info |
| `confirm.html` | `/confirm.html` | Legacy/alternate confirm path (writes primarily confirm on `write.html`) |

Shared assets:

- `style.css` — layout, cards, forms, nav, checkbox alignment
- `images/logo.png` — header logo

## Dynamic content: CPPCALL

Static HTML contains placeholders:

```html
<!--CPPCALL ShowHeaderStatus -->
```

At build time, `comphtml` binds these to C functions that write HTML fragments with `fdprintf`/`Write`. Implementations live in `web.cpp`.

Common CPPCALLs:

| Function | Used on | Output |
|----------|---------|--------|
| `ShowHeaderTarget` | All pages | Current server/database snippet |
| `ShowHeaderStatus` | All pages | Connection busy/idle indicator |
| `ShowQueryStatus` | Most pages | Last query message line |
| `ShowAutoRefresh` | Several | Meta refresh while SQL job running |
| `ShowFormScript` | Browse, Write | Embedded JavaScript for builder/preview |
| `ShowWriteTableControl` | Write | Table dropdown + refresh buttons |
| `ShowWritePageInit` | Write | Auto-load tables/columns when stale |

## Form POST handlers

NetBurner `HtmlPostVariableListCallback` registers action names. Important actions in `web.cpp`:

| POST action | Page | Effect |
|-------------|------|--------|
| `testconnection` | Configure | Request connection test job |
| `savesettings` | Configure | Validate + NV save |
| `browsedatabases` | Configure | Request database catalog job |
| `browsetables` | Browse / Write | Request table list for current DB |
| `browsecolumns` | Browse / Write | Request column list (Load Columns on Write) |
| `runquery` | Browse | Build or accept SELECT, request query job |
| `setwritetable` | Write | Set working table, redirect via `return_to` |
| `previewinsert` | Write | Stage INSERT SQL, show pending panel |
| `previewupdate` | Write | Stage UPDATE SQL, show pending panel |
| `previewwritesql` | Write | Stage custom write SQL |
| `confirmmutation` | Write / confirm | Execute staged mutation |
| `cancelmutation` | Write / confirm | Clear pending mutation |

Redirects after browse operations use `RedirectAfterBrowse()` with `return_to` query parameter so table refresh returns to Browse or Write as appropriate.

## Browse: Builder vs Custom SQL

Two modes toggled by buttons that add CSS classes to `<body>`:

- `nb-browse-mode-builder` — shows `#builder-panel`, hides `#custom-panel`
- `nb-browse-mode-custom` — opposite

JavaScript in `ShowFormScript` updates a live SQL preview textarea as builder fields change.

### comphtml and the `%` character

**Critical:** NetBurner's `comphtml` strips `%` from embedded C string literals used for JavaScript. A `%` inside `ShowFormScript` output breaks the entire script block.

For SQL `LIKE` wildcards in JS strings, use hex escape:

```javascript
// Wrong after comphtml: '%' + value + '%'
// Correct:
'\x25' + value + '\x25'
```

Symptom when broken: Builder/Custom toggle and filter append do nothing; browser console shows syntax errors.

## Write page workflow

1. **Working Table** section — database (read-only), table dropdown, **Refresh Tables**, **Load Columns**
2. On load, `ShowWritePageInit` auto-refreshes catalogs if table list or columns are missing/stale
3. User expands Guided Insert, Guided Update, or Custom Write
4. **Preview** POST stages SQL; pending panel appears on same page with **Confirm** / **Cancel**
5. Live preview textarea at bottom updates via client JS (`WebFormWritePreview` pattern in `ShowFormScript`)

Checkbox layout: CSS excludes checkboxes from full-width input rules:

```css
.nb-field input:not([type="checkbox"]) { width: 100%; }
```

Per-column "update this column" checkboxes were removed; blank fields are ignored on update.

## Results and export

`results.html` renders the last `sql_result_store` via CPPCALLs. **Export CSV** links to `/export.csv`, served by a dedicated handler that sets download headers and streams CSV from `SqlResultsExportCsv()`.

## Auto-refresh during jobs

`ShowAutoRefresh` emits an HTTP meta refresh (short interval) while `SqlRuntimeIsBusy()` is true so users see completion without manual reload.

## Remote console

`EnableRemoteConsole()` in `main.cpp` exposes `/console.html` — mirror of serial `iprintf` output. Use for TDS debug and connection errors when developing UI changes.

## Styling conventions

- `nb-header` / `nb-main-nav` — top branding and navigation
- `nb-card` — grouped form sections
- `nb-btn` / `nb-btn-secondary` — primary/secondary actions
- `nb-workflow-status` — status line under header
- Active nav link: `nb-nav-link-active` on current page

When adding pages, copy header/nav/footer structure from `index.html` for consistency.

## Adding a new page (checklist)

1. Create `html/newpage.html` with CPPCALL placeholders
2. Add CPPCALL implementations in `web.cpp`
3. Register HTML file in project makefile `HTMLFILES` (or equivalent comphtml list)
4. Add nav link on all pages
5. Rebuild — verify `htmldata.cpp` regenerates
6. Test on device; check `/console.html` for CPPCALL errors

## Security considerations

This example is intended for **trusted networks** (lab, plant floor VLAN):

- Credentials stored in NV flash on module
- HTTP only (no TLS in default NetBurner stack)
- SQL auth with configurable user — use least-privilege SQL logins in production
- Write path requires confirmation but is not a substitute for database-level permissions

Hardening for untrusted networks requires HTTPS termination elsewhere, VPN, and strict SQL roles — out of scope for the reference firmware but plan accordingly.
