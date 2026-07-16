// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>

#ifdef NB_GATEWAY_MICRO800

#include "gateway_web.h"
#include "gateway_config.h"
#include "gateway_plc_adapter.h"
#include "gateway_queue.h"
#include "gateway_runtime.h"
#include "gateway_sql.h"
#include "gateway_types.h"

#include "sql_runtime.h"
#include "sql_catalog.h"

#include <Micro800Client/micro800_client.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fdprintf.h>
#include <http.h>
#include <httppost.h>
#include <iosys.h>

namespace {

char g_status[160]{};
uint32_t g_edit_plc_id = 0;
uint32_t g_edit_map_id = 0;

gateway::PlcConfig g_post_plc{};
gateway::MappingConfig g_post_map{};
uint32_t g_post_delete_id = 0;
bool g_post_enable = false;
bool g_post_freq[8]{};
bool g_post_fkey[8]{};
char g_post_ftag[8][gateway::kMaxTagLen]{};
char g_post_fcol[8][gateway::kMaxSqlIdentLen]{};
int g_post_fptype[8]{};
int g_post_fstype[8]{};

// ListIdentity scan results (static — never on HTTP stack).
micro800::DiscoveredDevice g_scan_devices[micro800::kMaxDiscoveredDevices]{};
int g_scan_count = 0;
bool g_scan_ran = false;
char g_scan_err[96]{};
IPADDR4 g_post_scan_ip{};
char g_post_scan_name[gateway::kMaxNameLen]{};

enum { kMaxWebBrowseTags = 96 };
micro800::BrowsedTag g_tags[kMaxWebBrowseTags]{};
int g_tag_count = 0;
bool g_tag_browse_ran = false;
uint32_t g_tag_browse_plc_id = 0;
char g_tag_browse_err[96]{};
uint32_t g_post_browse_plc_id = 0;
uint32_t g_post_return_edit = 0;

char g_sticky_trigger[gateway::kMaxTagLen]{};
char g_sticky_ack[gateway::kMaxTagLen]{};
bool g_sticky_trigger_set = false;
bool g_sticky_ack_set = false;
char g_sticky_ftag[8][gateway::kMaxTagLen]{};
int g_sticky_fptype[8]{};
bool g_sticky_fset[8]{};

char g_post_use_tag[gateway::kMaxTagLen]{};
uint16_t g_post_use_symbol = 0;
char g_post_use_action[16]{};

char g_sticky_database[SQL_CFG_DATABASE_LEN]{};
char g_sticky_table[SQL_CFG_TABLE_LEN]{};
char g_sticky_schema[32]{};
bool g_sticky_dest_set = false;
char g_post_map_db[SQL_CFG_DATABASE_LEN]{};
char g_post_map_table[SQL_CFG_TABLE_LEN]{};
char g_post_map_schema[32]{};
char g_post_sql_browse_action[16]{};
bool g_post_return_new = false;

void SetStatus(const char *msg)
{
    if (!msg) {
        g_status[0] = '\0';
        return;
    }
    strncpy(g_status, msg, sizeof(g_status) - 1);
    g_status[sizeof(g_status) - 1] = '\0';
}

void HtmlEsc(int sock, const char *text)
{
    if (!text) {
        return;
    }
    char buf[128];
    size_t n = 0;
    auto flush = [&]() {
        if (n) {
            buf[n] = '\0';
            writestring(sock, buf);
            n = 0;
        }
    };
    for (const char *p = text; *p; ++p) {
        const char *rep = nullptr;
        switch (*p) {
        case '&':
            rep = "&amp;";
            break;
        case '<':
            rep = "&lt;";
            break;
        case '>':
            rep = "&gt;";
            break;
        case '"':
            rep = "&quot;";
            break;
        default:
            break;
        }
        if (rep) {
            flush();
            writestring(sock, rep);
        } else {
            if (n + 1 >= sizeof(buf)) {
                flush();
            }
            buf[n++] = *p;
        }
    }
    flush();
}

void CopyField(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

void NormalizeSqlIdentField(char *s)
{
    if (!s || !s[0]) {
        return;
    }
    const size_t len = strlen(s);
    if (len >= 2 && s[0] == '[' && s[len - 1] == ']') {
        memmove(s, s + 1, len - 2);
        s[len - 2] = '\0';
    }
    char *start = s;
    while (*start == ' ') {
        ++start;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == ' ') {
        s[--n] = '\0';
    }
}

uint32_t ParseU32(const char *s, uint32_t def = 0)
{
    if (!s || !s[0]) {
        return def;
    }
    return static_cast<uint32_t>(strtoul(s, nullptr, 10));
}

uint32_t UrlQueryU32(const char *url, const char *key)
{
    if (!url || !key) {
        return 0;
    }
    const char *q = strchr(url, '?');
    if (!q) {
        return 0;
    }
    char needle[32];
    sniprintf(needle, sizeof(needle), "%s=", key);
    const char *p = strstr(q + 1, needle);
    if (!p) {
        return 0;
    }
    p += strlen(needle);
    return ParseU32(p, 0);
}

bool UrlQueryHasFlag(const char *url, const char *key)
{
    if (!url || !key) {
        return false;
    }
    const char *q = strchr(url, '?');
    if (!q) {
        return false;
    }
    char needle[36];
    sniprintf(needle, sizeof(needle), "%s=", key);
    const char *p = strstr(q + 1, needle);
    if (!p) {
        return false;
    }
    p += strlen(needle);
    return (p[0] == '1' || p[0] == 't' || p[0] == 'T' || p[0] == 'y' || p[0] == 'Y');
}

int SqlTypeFromMssqlType(const char *typeName)
{
    if (!typeName || !typeName[0]) {
        return static_cast<int>(gateway::SqlDataType::Int);
    }
    char lower[SQL_CATALOG_TYPE_LEN]{};
    size_t n = 0;
    for (; typeName[n] && n + 1 < sizeof(lower); ++n) {
        const char c = typeName[n];
        lower[n] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    lower[n] = '\0';

    if (strstr(lower, "bit")) {
        return static_cast<int>(gateway::SqlDataType::Bit);
    }
    if (strstr(lower, "tinyint")) {
        return static_cast<int>(gateway::SqlDataType::TinyInt);
    }
    if (strstr(lower, "smallint")) {
        return static_cast<int>(gateway::SqlDataType::SmallInt);
    }
    if (strstr(lower, "bigint")) {
        return static_cast<int>(gateway::SqlDataType::BigInt);
    }
    if (strcmp(lower, "int") == 0 || strstr(lower, "integer")) {
        return static_cast<int>(gateway::SqlDataType::Int);
    }
    if (strstr(lower, "real")) {
        return static_cast<int>(gateway::SqlDataType::Real);
    }
    if (strstr(lower, "float") || strstr(lower, "double") || strstr(lower, "decimal") ||
        strstr(lower, "numeric") || strstr(lower, "money")) {
        return static_cast<int>(gateway::SqlDataType::Float);
    }
    if (strstr(lower, "nvarchar") || strstr(lower, "nchar") || strstr(lower, "ntext")) {
        return static_cast<int>(gateway::SqlDataType::NVarChar);
    }
    if (strstr(lower, "varchar") || strstr(lower, "char") || strstr(lower, "text") ||
        strstr(lower, "xml") || strstr(lower, "uniqueidentifier")) {
        return static_cast<int>(gateway::SqlDataType::VarChar);
    }
    return static_cast<int>(gateway::SqlDataType::NVarChar);
}

void WriteNameSelect(int sock, const char *fieldName, const char *selected,
                     const char names[][SQL_CATALOG_NAME_LEN], tdsl::uint32_t count, bool required,
                     const char *onchange = nullptr)
{
    fdprintf(sock, "<select name=\"%s\"%s", fieldName, required ? " required" : "");
    if (onchange && onchange[0]) {
        writestring(sock, " onchange=\"");
        writestring(sock, onchange);
        writestring(sock, "\"");
    }
    writestring(sock, ">");
    writestring(sock, "<option value=\"\">Select...</option>");
    bool found = false;
    for (tdsl::uint32_t i = 0; i < count; ++i) {
        const bool sel = selected && selected[0] && strcmp(selected, names[i]) == 0;
        if (sel) {
            found = true;
        }
        writestring(sock, "<option value=\"");
        HtmlEsc(sock, names[i]);
        fdprintf(sock, "\"%s>", sel ? " selected" : "");
        HtmlEsc(sock, names[i]);
        writestring(sock, "</option>");
    }
    if (selected && selected[0] && !found) {
        writestring(sock, "<option value=\"");
        HtmlEsc(sock, selected);
        writestring(sock, "\" selected>");
        HtmlEsc(sock, selected);
        writestring(sock, "</option>");
    }
    writestring(sock, "</select>");
}

void WriteNamedColumnSelect(int sock, const char *fieldName, const char *selected, const char *database,
                            const char *table, bool required)
{
    const sql_column_catalog &catalog = SqlColumnCatalogGet();
    const bool hasList = catalog.ready && catalog.success && catalog.column_count > 0 && database &&
                         database[0] && table && table[0] &&
                         strcmp(catalog.database, database) == 0 && strcmp(catalog.table, table) == 0;

    if (!hasList) {
        fdprintf(sock, "<input type=\"text\" name=\"%s\"%s value=\"", fieldName, required ? " required" : "");
        if (selected) {
            HtmlEsc(sock, selected);
        }
        writestring(sock, "\">");
        return;
    }

    fdprintf(sock, "<select name=\"%s\"%s>", fieldName, required ? " required" : "");
    writestring(sock, "<option value=\"\">Select column...</option>");
    bool found = false;
    for (tdsl::uint32_t i = 0; i < catalog.column_count; ++i) {
        const bool sel = selected && selected[0] && strcmp(selected, catalog.column_names[i]) == 0;
        if (sel) {
            found = true;
        }
        writestring(sock, "<option value=\"");
        HtmlEsc(sock, catalog.column_names[i]);
        fdprintf(sock, "\"%s>", sel ? " selected" : "");
        HtmlEsc(sock, catalog.column_names[i]);
        writestring(sock, " (");
        HtmlEsc(sock, catalog.column_types[i]);
        writestring(sock, ")</option>");
    }
    if (selected && selected[0] && !found) {
        writestring(sock, "<option value=\"");
        HtmlEsc(sock, selected);
        writestring(sock, "\" selected>");
        HtmlEsc(sock, selected);
        writestring(sock, "</option>");
    }
    writestring(sock, "</select>");
}

void WriteColumnSelect(int sock, unsigned row, const char *selected, const char *database,
                       const char *table)
{
    char fieldName[16]{};
    sniprintf(fieldName, sizeof(fieldName), "fcol%u", row);
    WriteNamedColumnSelect(sock, fieldName, selected, database, table, false);
}

bool ColumnsCatalogReady(const char *database, const char *table)
{
    const sql_column_catalog &catalog = SqlColumnCatalogGet();
    return catalog.ready && catalog.success && catalog.column_count > 0 && database && database[0] &&
           table && table[0] && strcmp(catalog.database, database) == 0 &&
           strcmp(catalog.table, table) == 0;
}

bool LookupCatalogSqlType(const char *database, const char *table, const char *column, int *outType)
{
    if (!outType || !column || !column[0] || !ColumnsCatalogReady(database, table)) {
        return false;
    }
    const sql_column_catalog &catalog = SqlColumnCatalogGet();
    for (tdsl::uint32_t i = 0; i < catalog.column_count; ++i) {
        if (strcmp(catalog.column_names[i], column) == 0) {
            *outType = SqlTypeFromMssqlType(catalog.column_types[i]);
            return true;
        }
    }
    return false;
}

bool LookupPlcTypeFromBrowse(const char *tag, int *outType)
{
    if (!outType || !tag || !tag[0] || g_tag_count <= 0) {
        return false;
    }
    for (int i = 0; i < g_tag_count; ++i) {
        if (strcmp(g_tags[i].name, tag) != 0) {
            continue;
        }
        const gateway::PlcDataType t = gateway::PlcTypeFromSymbol(g_tags[i].symbolType);
        if (t == gateway::PlcDataType::Unknown) {
            return false;
        }
        *outType = static_cast<int>(t);
        return true;
    }
    return false;
}

bool TagBrowseReady()
{
    return g_tag_browse_ran && g_tag_count > 0;
}

void WriteTagSelect(int sock, const char *fieldName, const char *selected, bool boolOnly, bool required)
{
    if (!TagBrowseReady()) {
        fdprintf(sock, "<input type=\"text\" name=\"%s\"%s value=\"", fieldName, required ? " required" : "");
        if (selected) {
            HtmlEsc(sock, selected);
        }
        writestring(sock, "\">");
        return;
    }

    fdprintf(sock, "<select name=\"%s\"%s>", fieldName, required ? " required" : "");
    writestring(sock, "<option value=\"\">Select tag...</option>");
    bool found = false;
    for (int i = 0; i < g_tag_count; ++i) {
        const micro800::BrowsedTag &t = g_tags[i];
        if (!t.name[0]) {
            continue;
        }
        const gateway::PlcDataType pt = gateway::PlcTypeFromSymbol(t.symbolType);
        if (boolOnly) {
            if (pt != gateway::PlcDataType::Bool || t.isArray) {
                continue;
            }
        }
        const bool sel = selected && selected[0] && strcmp(selected, t.name) == 0;
        if (sel) {
            found = true;
        }
        const uint16_t typeCode = static_cast<uint16_t>(t.symbolType & 0xFFFu);
        const char *typeName = micro800::CipTypeName(typeCode);
        writestring(sock, "<option value=\"");
        HtmlEsc(sock, t.name);
        fdprintf(sock, "\"%s>", sel ? " selected" : "");
        HtmlEsc(sock, t.name);
        writestring(sock, " (");
        HtmlEsc(sock, typeName && typeName[0] ? typeName : "?");
        writestring(sock, ")</option>");
    }
    if (selected && selected[0] && !found) {
        writestring(sock, "<option value=\"");
        HtmlEsc(sock, selected);
        writestring(sock, "\" selected>");
        HtmlEsc(sock, selected);
        writestring(sock, "</option>");
    }
    writestring(sock, "</select>");
}

const char *PlcTypeName(int type)
{
    switch (type) {
    case 0:
        return "BOOL";
    case 1:
        return "SINT";
    case 2:
        return "INT";
    case 3:
        return "DINT";
    case 4:
        return "REAL";
    case 5:
        return "STRING";
    case 7:
        return "USINT";
    case 8:
        return "UINT";
    case 9:
        return "UDINT";
    case 10:
        return "LINT";
    case 11:
        return "ULINT";
    case 12:
        return "LREAL";
    default:
        return "?";
    }
}

void ShowSqlDestBrowseStatus(int sock, const char *database, const char *table)
{
    if (SqlRuntimeIsBrowsingDatabases()) {
        writestring(sock, "<p class=\"nb-field-hint\">Loading databases...</p>");
        return;
    }
    if (SqlRuntimeIsBrowsingTables()) {
        writestring(sock, "<p class=\"nb-field-hint\">Loading tables...</p>");
        return;
    }
    if (SqlRuntimeIsBrowsingColumns()) {
        writestring(sock, "<p class=\"nb-field-hint\">Loading columns...</p>");
        return;
    }

    const sql_database_catalog &dbs = SqlDatabaseCatalogGet();
    const sql_table_catalog &tables = SqlCatalogGet();
    const sql_column_catalog &cols = SqlColumnCatalogGet();

    writestring(sock, "<p class=\"nb-field-hint\">");
    if (dbs.ready && dbs.success) {
        fdprintf(sock, "%lu database(s)", static_cast<unsigned long>(dbs.database_count));
    } else if (dbs.ready && !dbs.success) {
        writestring(sock, "Databases: ");
        HtmlEsc(sock, dbs.status);
    } else {
        writestring(sock, "Refresh Databases to start");
    }

    if (tables.ready && tables.success && database && database[0] &&
        strcmp(tables.database, database) == 0) {
        fdprintf(sock, " · %lu table(s)", static_cast<unsigned long>(tables.table_count));
    } else if (database && database[0]) {
        writestring(sock, " · pick a database to load tables");
    }

    if (cols.ready && cols.success && database && table && database[0] && table[0] &&
        SqlColumnCatalogMatches(database, table)) {
        fdprintf(sock, " · %lu column(s)", static_cast<unsigned long>(cols.column_count));
    } else if (table && table[0]) {
        writestring(sock, " · columns loading for selected table");
    }
    writestring(sock, "</p>");
}

void MaybeAutoloadSqlCatalog(const char *database, const char *table)
{
    if (SqlRuntimeIsBusy() || !database || !database[0]) {
        return;
    }

    sql_runtime_config sqlCfg = SqlRuntimeGetConfig();
    bool dirty = false;
    if (strcmp(sqlCfg.database, database) != 0) {
        CopyField(sqlCfg.database, sizeof(sqlCfg.database), database);
        dirty = true;
    }
    if (table && table[0] && strcmp(sqlCfg.table, table) != 0) {
        CopyField(sqlCfg.table, sizeof(sqlCfg.table), table);
        dirty = true;
    } else if ((!table || !table[0]) && sqlCfg.table[0]) {
        sqlCfg.table[0] = '\0';
        dirty = true;
    }
    if (dirty) {
        SqlRuntimeUpdateConfig(sqlCfg);
    }

    const sql_table_catalog &tables = SqlCatalogGet();
    if (tables.ready && strcmp(tables.database, database) == 0) {
        // Already attempted this database (success or fail) — don't spin.
    } else if (!tables.ready || !tables.success || !SqlCatalogTablesMatchDatabase(database)) {
        SqlRuntimeRequestBrowseTables();
        SetStatus("Loading tables...");
        return;
    }

    if (!table || !table[0]) {
        return;
    }

    const sql_column_catalog &cols = SqlColumnCatalogGet();
    if (cols.ready && strcmp(cols.database, database) == 0 && strcmp(cols.table, table) == 0) {
        // Already attempted this table (success or fail) — don't spin.
        return;
    }

    if (!SqlColumnCatalogMatches(database, table)) {
        SqlRuntimeRequestBrowseColumns();
        SetStatus("Loading columns...");
    }
}

void ResolveMapDestination(const gateway::MappingConfig *edit, char *db, size_t dbCap, char *schema,
                           size_t schemaCap, char *table, size_t tableCap)
{
    const sql_runtime_config &sqlCfg = SqlRuntimeGetConfig();
    if (g_sticky_dest_set && g_sticky_database[0]) {
        CopyField(db, dbCap, g_sticky_database);
    } else if (edit && edit->database[0]) {
        CopyField(db, dbCap, edit->database);
    } else {
        CopyField(db, dbCap, sqlCfg.database);
    }

    if (g_sticky_dest_set && g_sticky_schema[0]) {
        CopyField(schema, schemaCap, g_sticky_schema);
    } else if (edit && edit->schema[0]) {
        CopyField(schema, schemaCap, edit->schema);
    } else {
        CopyField(schema, schemaCap, "dbo");
    }

    if (g_sticky_dest_set && g_sticky_table[0]) {
        CopyField(table, tableCap, g_sticky_table);
    } else if (edit && edit->table[0]) {
        CopyField(table, tableCap, edit->table);
    } else {
        CopyField(table, tableCap, sqlCfg.table);
    }
}

uint32_t NextPlcId(const gateway::GatewayConfig &cfg)
{
    uint32_t m = 0;
    for (uint16_t i = 0; i < cfg.plc_count; ++i) {
        if (cfg.plcs[i].id > m) {
            m = cfg.plcs[i].id;
        }
    }
    return m + 1;
}

uint32_t NextMapId(const gateway::GatewayConfig &cfg)
{
    uint32_t m = 0;
    for (uint16_t i = 0; i < cfg.mapping_count; ++i) {
        if (cfg.mappings[i].id > m) {
            m = cfg.mappings[i].id;
        }
    }
    return m + 1;
}

bool ParseIpv4(const char *text, IPADDR4 &out)
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (!text || sscanf(text, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        return false;
    }
    if (a > 255 || b > 255 || c > 255 || d > 255) {
        return false;
    }
    out = IPV4FromConst((a << 24) | (b << 16) | (c << 8) | d);
    return true;
}

void WriteTypeOptions(int sock, int selected, bool plc)
{
    if (plc) {
        // Values must match gateway::PlcDataType (Unknown=6 is omitted from the UI).
        const struct {
            int value;
            const char *name;
        } opts[] = {{0, "BOOL"},  {1, "SINT"},  {7, "USINT"}, {2, "INT"},   {8, "UINT"},
                    {3, "DINT"},  {9, "UDINT"}, {10, "LINT"}, {11, "ULINT"}, {4, "REAL"},
                    {12, "LREAL"}, {5, "STRING"}};
        for (unsigned i = 0; i < sizeof(opts) / sizeof(opts[0]); ++i) {
            fdprintf(sock, "<option value=\"%d\"%s>%s</option>", opts[i].value,
                     selected == opts[i].value ? " selected" : "", opts[i].name);
        }
    } else {
        // Values must match gateway::SqlDataType (Unknown=8 omitted).
        const struct {
            int value;
            const char *name;
        } opts[] = {{0, "BIT"},      {1, "TINYINT"}, {2, "SMALLINT"}, {3, "INT"},
                    {9, "BIGINT"},   {4, "REAL"},    {5, "FLOAT"},    {6, "NVARCHAR"},
                    {7, "VARCHAR"}};
        for (unsigned i = 0; i < sizeof(opts) / sizeof(opts[0]); ++i) {
            fdprintf(sock, "<option value=\"%d\"%s>%s</option>", opts[i].value,
                     selected == opts[i].value ? " selected" : "", opts[i].name);
        }
    }
}

bool PersistConfig(gateway::GatewayConfig &cfg, char *err, size_t errCap)
{
    // Validate before bumping revision so a rejected save cannot desync RAM vs NV
    // or leave the handshake runtime unreloaded.
    if (!gateway::ConfigValidate(cfg, err, errCap)) {
        return false;
    }
    ++cfg.config_revision;
    if (!gateway::ConfigSave(cfg, err, errCap)) {
        return false;
    }
    gateway::RuntimeReloadFromConfig();
    return true;
}

bool PlcIpConfigured(const gateway::GatewayConfig &cfg, IPADDR4 ip)
{
    const uint32_t raw = static_cast<uint32_t>(ip);
    if (raw == 0) {
        return false;
    }
    for (uint16_t i = 0; i < cfg.plc_count; ++i) {
        if (static_cast<uint32_t>(cfg.plcs[i].ip) == raw) {
            return true;
        }
    }
    return false;
}

void FormatIpv4(char *dst, size_t cap, IPADDR4 ip)
{
    if (!dst || cap < 8) {
        return;
    }
    const uint32_t raw = static_cast<uint32_t>(ip);
    sniprintf(dst, cap, "%u.%u.%u.%u", (raw >> 24) & 0xFF, (raw >> 16) & 0xFF, (raw >> 8) & 0xFF,
              raw & 0xFF);
}

void ShowStatusBanner(int sock)
{
    if (!g_status[0]) {
        return;
    }
    writestring(sock, "<p class=\"nb-field-hint\">");
    HtmlEsc(sock, g_status);
    writestring(sock, "</p>");
}

void ClearTagStickies()
{
    g_sticky_trigger[0] = '\0';
    g_sticky_ack[0] = '\0';
    g_sticky_trigger_set = false;
    g_sticky_ack_set = false;
    for (int i = 0; i < 8; ++i) {
        g_sticky_ftag[i][0] = '\0';
        g_sticky_fptype[i] = 3;
        g_sticky_fset[i] = false;
    }
    g_sticky_database[0] = '\0';
    g_sticky_table[0] = '\0';
    g_sticky_schema[0] = '\0';
    g_sticky_dest_set = false;
}

void RedirectSqlMaps(int sock, uint32_t editId, bool wantNew = false, const char *frag = nullptr,
                     bool openTags = false)
{
    char redir[128]{};
    if (wantNew && openTags) {
        sniprintf(redir, sizeof(redir), "sql_maps.html?new=1&tags=1");
    } else if (wantNew) {
        sniprintf(redir, sizeof(redir), "sql_maps.html?new=1");
    } else if (editId && openTags) {
        sniprintf(redir, sizeof(redir), "sql_maps.html?edit=%lu&tags=1",
                  static_cast<unsigned long>(editId));
    } else if (editId) {
        sniprintf(redir, sizeof(redir), "sql_maps.html?edit=%lu", static_cast<unsigned long>(editId));
    } else if (openTags) {
        sniprintf(redir, sizeof(redir), "sql_maps.html?tags=1");
    } else {
        sniprintf(redir, sizeof(redir), "sql_maps.html");
    }
    if (frag && frag[0]) {
        const size_t n = strlen(redir);
        if (n + 1 + strlen(frag) < sizeof(redir)) {
            redir[n] = '#';
            strncpy(redir + n + 1, frag, sizeof(redir) - n - 2);
            redir[sizeof(redir) - 1] = '\0';
        }
    }
    RedirectResponse(sock, redir);
}

int PlcTypeOptionFromSymbol(uint16_t symbolType)
{
    const gateway::PlcDataType t = gateway::PlcTypeFromSymbol(symbolType);
    if (t == gateway::PlcDataType::Unknown) {
        return 3; // DINT default in form
    }
    return static_cast<int>(t);
}

void WriteTagUseForm(int sock, uint32_t editMapId, bool wantNew, const char *action, const char *label,
                     const char *tag, uint16_t symbolType)
{
    writestring(sock, "<form class=\"nb-inline-form\" method=\"post\" action=\"gateway_tag_use\" "
                      "style=\"display:inline;margin-right:0.25rem\">");
    fdprintf(sock, "<input type=\"hidden\" name=\"edit\" value=\"%lu\">", static_cast<unsigned long>(editMapId));
    if (wantNew) {
        writestring(sock, "<input type=\"hidden\" name=\"new\" value=\"1\">");
    }
    fdprintf(sock, "<input type=\"hidden\" name=\"action\" value=\"%s\">", action ? action : "");
    fdprintf(sock, "<input type=\"hidden\" name=\"symbol\" value=\"%u\">", static_cast<unsigned>(symbolType));
    writestring(sock, "<input type=\"hidden\" name=\"tag\" value=\"");
    HtmlEsc(sock, tag);
    writestring(sock, "\">");
    fdprintf(sock, "<button type=\"submit\" class=\"nb-btn nb-btn-secondary nb-btn-inline\">%s</button></form>",
             label ? label : "Use");
}

void ShowTagBrowser(int sock, const gateway::GatewayConfig &cfg, uint32_t preferredPlcId, uint32_t editMapId,
                    bool wantNew, bool openModal)
{
    if (cfg.plc_count == 0) {
        return;
    }

    fdprintf(sock,
             "<div id=\"nb-tag-modal\" class=\"nb-modal-overlay%s\"%s role=\"dialog\" "
             "aria-modal=\"true\" aria-labelledby=\"nb-tag-modal-title\">",
             openModal ? " nb-modal-open" : "", openModal ? "" : " hidden");
    writestring(sock, "<div class=\"nb-modal\" style=\"max-width:920px\">");
    writestring(sock, "<div class=\"nb-modal-header\">"
                      "<h3 id=\"nb-tag-modal-title\">Browse PLC tags</h3>"
                      "<button type=\"button\" class=\"nb-modal-close\" id=\"nb-close-tag-modal\" "
                      "aria-label=\"Close\">&times;</button></div>");
    writestring(sock, "<div class=\"nb-modal-body\">");

    writestring(sock, "<form class=\"nb-form\" method=\"post\" action=\"gateway_tag_browse\">");
    fdprintf(sock, "<input type=\"hidden\" name=\"edit\" value=\"%lu\">", static_cast<unsigned long>(editMapId));
    if (wantNew) {
        writestring(sock, "<input type=\"hidden\" name=\"new\" value=\"1\">");
    }
    writestring(sock, "<div class=\"nb-form-grid\">");
    writestring(sock, "<label class=\"nb-field\"><span>PLC</span><select name=\"plc_id\">");
    for (uint16_t i = 0; i < cfg.plc_count; ++i) {
        const uint32_t id = cfg.plcs[i].id;
        const bool sel = (preferredPlcId ? id == preferredPlcId
                                         : (g_tag_browse_plc_id ? id == g_tag_browse_plc_id : i == 0));
        fdprintf(sock, "<option value=\"%lu\"%s>", static_cast<unsigned long>(id), sel ? " selected" : "");
        HtmlEsc(sock, cfg.plcs[i].name);
        writestring(sock, "</option>");
    }
    writestring(sock, "</select></label></div>");
    writestring(sock, "<div class=\"nb-form-actions\">"
                      "<button type=\"submit\" class=\"nb-btn\">Browse Tags</button></div>"
                      "<p class=\"nb-field-hint\">CIP List Tags on the selected PLC. Use Trigger / ACK / Field "
                      "to fill the mapping form behind this dialog.</p></form>");

    if (!g_tag_browse_ran) {
        writestring(sock, "</div></div></div>");
        return;
    }

    if (g_tag_browse_err[0]) {
        writestring(sock, "<p class=\"nb-field-hint\">");
        HtmlEsc(sock, g_tag_browse_err);
        writestring(sock, "</p>");
    }

    if (g_tag_count <= 0) {
        writestring(sock, "<p class=\"nb-field-hint\">No tags returned.</p></div></div></div>");
        return;
    }

    writestring(sock, "<div style=\"max-height:50vh;overflow:auto;margin-top:0.75rem\">");
    writestring(sock, "<table class=\"nb-table\"><thead><tr>"
                      "<th>Tag</th><th>Type</th><th>Array</th><th></th></tr></thead><tbody>");
    for (int i = 0; i < g_tag_count; ++i) {
        const micro800::BrowsedTag &t = g_tags[i];
        if (!t.name[0]) {
            continue;
        }
        const uint16_t typeCode = static_cast<uint16_t>(t.symbolType & 0xFFFu);
        const char *typeName = micro800::CipTypeName(typeCode);
        const bool isBool = (gateway::PlcTypeFromSymbol(t.symbolType) == gateway::PlcDataType::Bool);

        writestring(sock, "<tr><td>");
        HtmlEsc(sock, t.name);
        fdprintf(sock, "</td><td>%s (0x%03X)</td><td>%s</td><td>", typeName && typeName[0] ? typeName : "?",
                 static_cast<unsigned>(typeCode), t.isArray ? "yes" : "no");

        if (isBool && !t.isArray) {
            WriteTagUseForm(sock, editMapId, wantNew, "trigger", "Trigger", t.name, t.symbolType);
            WriteTagUseForm(sock, editMapId, wantNew, "ack", "ACK", t.name, t.symbolType);
        }
        WriteTagUseForm(sock, editMapId, wantNew, "field", "Field", t.name, t.symbolType);
        writestring(sock, "</td></tr>");
    }
    writestring(sock, "</tbody></table></div>");
    writestring(sock, "<div class=\"nb-form-actions\" style=\"margin-top:1rem\">"
                      "<button type=\"button\" class=\"nb-btn\" id=\"nb-done-tag-modal\">Done</button>"
                      "</div>");
    writestring(sock, "</div></div></div>");
}

void ShowScanResults(int sock, const gateway::GatewayConfig &cfg)
{
    writestring(sock, "<div class=\"nb-card\" style=\"margin-top:1rem\"><div class=\"nb-card-header\">"
                      "<span>Network scan (EtherNet/IP ListIdentity)</span></div>");
    writestring(sock, "<form class=\"nb-form\" method=\"post\" action=\"gateway_plc_scan\">"
                      "<div class=\"nb-form-actions\">"
                      "<button type=\"submit\" class=\"nb-btn\">Scan for PLCs</button></div>"
                      "<p class=\"nb-field-hint\">UDP broadcast (~3s). Micro800 candidates are highlighted.</p>"
                      "</form>");

    if (!g_scan_ran) {
        writestring(sock, "</div>");
        return;
    }

    if (g_scan_err[0]) {
        writestring(sock, "<p class=\"nb-field-hint\">");
        HtmlEsc(sock, g_scan_err);
        writestring(sock, "</p>");
    }

    if (g_scan_count <= 0) {
        writestring(sock, "<p class=\"nb-field-hint\">No EtherNet/IP devices responded.</p></div>");
        return;
    }

    writestring(sock, "<table class=\"nb-table\"><thead><tr>"
                      "<th>IP</th><th>Product</th><th>Vendor</th><th>Micro800</th><th></th>"
                      "</tr></thead><tbody>");
    for (int i = 0; i < g_scan_count; ++i) {
        const micro800::DiscoveredDevice &d = g_scan_devices[i];
        if (!d.valid) {
            continue;
        }
        const bool isM8 = micro800::LooksLikeMicro800(d);
        char ipBuf[24]{};
        FormatIpv4(ipBuf, sizeof(ipBuf), d.ip);
        const bool already = PlcIpConfigured(cfg, d.ip);

        writestring(sock, "<tr>");
        fdprintf(sock, "<td>%s</td><td>", ipBuf);
        HtmlEsc(sock, d.productName[0] ? d.productName : "(unknown)");
        fdprintf(sock, "</td><td>0x%04X</td><td>%s</td><td>", static_cast<unsigned>(d.vendorId),
                 isM8 ? "yes" : "no");
        if (already) {
            writestring(sock, "<span class=\"nb-field-hint\">Configured</span>");
        } else {
            writestring(sock, "<form class=\"nb-inline-form\" method=\"post\" action=\"gateway_plc_add_scan\" "
                              "style=\"display:inline\">");
            fdprintf(sock, "<input type=\"hidden\" name=\"ip\" value=\"%s\">", ipBuf);
            writestring(sock, "<input type=\"hidden\" name=\"name\" value=\"");
            HtmlEsc(sock, d.productName[0] ? d.productName : ipBuf);
            writestring(sock, "\">");
            writestring(sock, "<button type=\"submit\" class=\"nb-btn nb-btn-inline\">Add</button></form>");
        }
        writestring(sock, "</td></tr>");
    }
    writestring(sock, "</tbody></table></div>");
}

} // namespace

void GatewayWebInit()
{
    SetStatus("");
}

void ShowAppNav(int sock, PCSTR url)
{
    auto writeLink = [](int s, PCSTR u, const char *href, const char *label) {
        const bool active = (u && strstr(u, href) != nullptr);
        fdprintf(s, "<a class=\"nb-nav-link%s\" href=\"%s\">%s</a>", active ? " nb-nav-link-active" : "", href,
                 label);
    };

    const bool onToolPage =
        url && (strstr(url, "configure") || strstr(url, "diagnostic") || strstr(url, "gateway.html") ||
                strstr(url, "plc.html") || strstr(url, "sql_maps.html"));
    fdprintf(sock, "<a class=\"nb-nav-link%s\" href=\"index.html\">Dashboard</a>",
             onToolPage ? "" : " nb-nav-link-active");
    writeLink(sock, url, "configure.html", "SQL Server");
    writeLink(sock, url, "gateway.html", "Gateway");
    writeLink(sock, url, "plc.html", "PLCs");
    writeLink(sock, url, "sql_maps.html", "SQL Maps");
    writeLink(sock, url, "diagnostics.html", "Diagnostics");
}

void ShowGatewayNavLinks(int sock, PCSTR url)
{
    // Kept for older HTML; gateway nav is emitted by ShowAppNav.
    (void)sock;
    (void)url;
}

void ShowGatewayOverview(int sock, PCSTR /*url*/)
{
    const gateway::GatewayConfig &cfg = gateway::ConfigGet();
    const gateway::QueueStats qs = gateway::QueueGetStats();

    ShowStatusBanner(sock);

    writestring(sock, "<form class=\"nb-form\" method=\"post\" action=\"gateway_enable\">");
    fdprintf(sock, "<label class=\"nb-field\"><span>Runtime enabled</span>"
                   "<select name=\"enabled\"><option value=\"1\"%s>Yes</option>"
                   "<option value=\"0\"%s>No</option></select></label>",
             cfg.enabled ? " selected" : "", cfg.enabled ? "" : " selected");
    writestring(sock, "<div class=\"nb-form-actions\">"
                      "<button type=\"submit\" class=\"nb-btn\">Save Enable</button></div></form>");
    fdprintf(sock, "<p>Handshake polling: <strong>%s</strong></p>",
             gateway::RuntimeIsActive() ? "ACTIVE" : "STOPPED");

    fdprintf(sock, "<p>PLCs: %u &nbsp; Mappings: %u &nbsp; Config rev: %lu</p>",
             static_cast<unsigned>(cfg.plc_count), static_cast<unsigned>(cfg.mapping_count),
             static_cast<unsigned long>(cfg.config_revision));
    fdprintf(sock, "<p>Queue backend: <strong>%s</strong> &nbsp; pressure: %s</p>",
             qs.backend == gateway::QueueBackend::Effs ? "EFFS" : "RAM",
             qs.pressure == gateway::QueuePressure::Full       ? "FULL"
             : qs.pressure == gateway::QueuePressure::Critical ? "CRITICAL"
             : qs.pressure == gateway::QueuePressure::Warning  ? "WARNING"
             : qs.pressure == gateway::QueuePressure::Fault    ? "FAULT"
                                                              : "normal");
    fdprintf(sock, "<p>Queue bytes: %lu / %lu &nbsp; pending: %lu / %lu &nbsp; quarantined(total): %lu &nbsp; "
                   "full-drops: %lu</p>",
             static_cast<unsigned long>(qs.bytes_used), static_cast<unsigned long>(qs.bytes_budget),
             static_cast<unsigned long>(qs.pending), static_cast<unsigned long>(qs.pending_capacity),
             static_cast<unsigned long>(qs.quarantined),
             static_cast<unsigned long>(qs.dropped_or_full));
    fdprintf(sock, "<p>Next event id: %llu &nbsp; Inflight SQL: %s</p>",
             static_cast<unsigned long long>(qs.next_event_id), gateway::HasInflightEvent() ? "yes" : "no");

    writestring(sock, "<table class=\"nb-table\"><thead><tr><th>Mapping</th><th>State</th>"
                      "<th>Fires</th><th>Errors</th><th>Last error</th></tr></thead><tbody>");
    for (uint16_t i = 0; i < cfg.mapping_count; ++i) {
        const gateway::MappingConfig &m = cfg.mappings[i];
        const gateway::MappingRuntime *rt = gateway::RuntimeFindConst(m.id);
        fdprintf(sock, "<tr><td>");
        HtmlEsc(sock, m.name);
        fdprintf(sock, "</td><td>%s</td><td>%lu</td><td>%lu</td><td>",
                 rt ? gateway::HandshakeStateName(rt->state) : "-",
                 rt ? static_cast<unsigned long>(rt->fire_count) : 0ul,
                 rt ? static_cast<unsigned long>(rt->error_count) : 0ul);
        HtmlEsc(sock, rt && rt->last_error[0] ? rt->last_error : "");
        writestring(sock, "</td></tr>");
    }
    writestring(sock, "</tbody></table>");
}

void ShowPlcManager(int sock, PCSTR url)
{
    const gateway::GatewayConfig &cfg = gateway::ConfigGet();
    const uint32_t editId = UrlQueryU32(url, "edit");
    const gateway::PlcConfig *edit = nullptr;
    for (uint16_t i = 0; i < cfg.plc_count; ++i) {
        if (cfg.plcs[i].id == editId || (editId == 0 && g_edit_plc_id && cfg.plcs[i].id == g_edit_plc_id)) {
            edit = &cfg.plcs[i];
            break;
        }
    }
    const bool haveEdit = (edit != nullptr);

    ShowStatusBanner(sock);
    ShowScanResults(sock, cfg);

    writestring(sock, "<div class=\"nb-card-header\" style=\"margin-top:1rem\"><span>Saved PLCs</span></div>");
    writestring(sock, "<div class=\"nb-table-wrap\" style=\"max-height:none;margin:0 0 1rem\">");
    writestring(sock, "<table class=\"nb-table\"><thead><tr>"
                      "<th class=\"nb-col-id\">ID</th>"
                      "<th class=\"nb-col-name\">Name</th>"
                      "<th class=\"nb-col-ip\">IP</th>"
                      "<th class=\"nb-col-status\">Enabled</th>"
                      "<th class=\"nb-col-num\">Poll (ms)</th>"
                      "<th class=\"nb-col-actions\"></th>"
                      "</tr></thead><tbody>");
    for (uint16_t i = 0; i < cfg.plc_count; ++i) {
        const gateway::PlcConfig &p = cfg.plcs[i];
        const uint32_t raw = static_cast<uint32_t>(p.ip);
        fdprintf(sock, "<tr><td class=\"nb-col-id\">%lu</td><td class=\"nb-col-name\">",
                 static_cast<unsigned long>(p.id));
        HtmlEsc(sock, p.name);
        fdprintf(sock,
                 "</td><td class=\"nb-col-ip\">%u.%u.%u.%u</td>"
                 "<td class=\"nb-col-status\"><span class=\"nb-pill %s\">%s</span></td>"
                 "<td class=\"nb-col-num\">%lu</td><td class=\"nb-col-actions\">",
                 (raw >> 24) & 0xFF, (raw >> 16) & 0xFF, (raw >> 8) & 0xFF, raw & 0xFF,
                 p.enabled ? "nb-pill-ok" : "nb-pill-off", p.enabled ? "Yes" : "No",
                 static_cast<unsigned long>(p.poll_interval_ms));
        fdprintf(sock,
                 "<a class=\"nb-btn nb-btn-secondary nb-btn-inline\" href=\"plc.html?edit=%lu\">Edit</a>",
                 static_cast<unsigned long>(p.id));
        writestring(sock, "<form class=\"nb-inline-form\" method=\"post\" action=\"gateway_plc_delete\">");
        fdprintf(sock, "<input type=\"hidden\" name=\"id\" value=\"%lu\">",
                 static_cast<unsigned long>(p.id));
        writestring(sock, "<button type=\"submit\" class=\"nb-btn nb-btn-danger nb-btn-inline\">"
                          "Delete</button></form>");
        writestring(sock, "</td></tr>");
    }
    if (cfg.plc_count == 0) {
        writestring(sock, "<tr><td colspan=\"6\" class=\"nb-field-hint\">No PLCs configured yet. "
                          "Scan the network or add one manually below.</td></tr>");
    }
    writestring(sock, "</tbody></table></div>");

    writestring(sock, "<div class=\"nb-card\" style=\"margin-top:1rem\"><div class=\"nb-card-header\"><span>");
    writestring(sock, haveEdit ? "Edit PLC" : "Add PLC manually");
    writestring(sock, "</span></div>");
    writestring(sock, "<form class=\"nb-form\" method=\"post\" action=\"gateway_plc_save\">");
    fdprintf(sock, "<input type=\"hidden\" name=\"id\" value=\"%lu\">",
             static_cast<unsigned long>(haveEdit ? edit->id : 0));
    writestring(sock, "<div class=\"nb-form-grid\">");
    writestring(sock, "<label class=\"nb-field\"><span>Name</span><input type=\"text\" name=\"name\" required value=\"");
    if (haveEdit) {
        HtmlEsc(sock, edit->name);
    }
    writestring(sock, "\"></label>");
    writestring(sock, "<label class=\"nb-field\"><span>IP</span><input type=\"text\" name=\"ip\" required "
                      "placeholder=\"192.168.1.10\" value=\"");
    if (haveEdit) {
        const uint32_t raw = static_cast<uint32_t>(edit->ip);
        fdprintf(sock, "%u.%u.%u.%u", (raw >> 24) & 0xFF, (raw >> 16) & 0xFF, (raw >> 8) & 0xFF, raw & 0xFF);
    }
    writestring(sock, "\"></label>");
    fdprintf(sock,
             "<label class=\"nb-field\"><span>Enabled</span><select name=\"enabled\">"
             "<option value=\"1\"%s>Yes</option><option value=\"0\"%s>No</option></select></label>",
             (!haveEdit || edit->enabled) ? " selected" : "", (haveEdit && !edit->enabled) ? " selected" : "");
    fdprintf(sock, "<label class=\"nb-field\"><span>Poll interval (ms)</span>"
                   "<input type=\"number\" name=\"poll_ms\" min=\"50\" value=\"%lu\"></label>",
             static_cast<unsigned long>(haveEdit ? edit->poll_interval_ms : 200));
    fdprintf(sock, "<label class=\"nb-field\"><span>Request timeout (ms)</span>"
                   "<input type=\"number\" name=\"timeout_ms\" min=\"100\" value=\"%lu\"></label>",
             static_cast<unsigned long>(haveEdit ? edit->request_timeout_ms : 3000));
    fdprintf(sock, "<label class=\"nb-field\"><span>Retry delay (ms)</span>"
                   "<input type=\"number\" name=\"retry_ms\" min=\"50\" value=\"%lu\"></label>",
             static_cast<unsigned long>(haveEdit ? edit->retry_delay_ms : 500));
    writestring(sock, "</div><div class=\"nb-form-actions\">"
                      "<button type=\"submit\" class=\"nb-btn\">Save PLC</button></div></form></div>");
}

void EmitSqlMapsBrowseRefresh(int sock, PCSTR url)
{
    const uint32_t editId = UrlQueryU32(url, "edit");
    const bool wantNew = UrlQueryHasFlag(url, "new");
    const bool keepTags = UrlQueryHasFlag(url, "tags");

    uint32_t refreshEdit = editId;
    if (!refreshEdit && !wantNew) {
        if (g_edit_map_id) {
            refreshEdit = g_edit_map_id;
        } else if (gateway::ConfigGet().mapping_count == 1) {
            refreshEdit = gateway::ConfigGet().mappings[0].id;
        }
    }

    char target[128]{};
    if (wantNew) {
        if (keepTags) {
            sniprintf(target, sizeof(target), "sql_maps.html?new=1&tags=1");
        } else {
            sniprintf(target, sizeof(target), "sql_maps.html?new=1#sql-dest");
        }
    } else if (refreshEdit) {
        if (keepTags) {
            sniprintf(target, sizeof(target), "sql_maps.html?edit=%lu&tags=1",
                      static_cast<unsigned long>(refreshEdit));
        } else {
            sniprintf(target, sizeof(target), "sql_maps.html?edit=%lu#sql-dest",
                      static_cast<unsigned long>(refreshEdit));
        }
    } else if (keepTags) {
        sniprintf(target, sizeof(target), "sql_maps.html?tags=1");
    } else {
        sniprintf(target, sizeof(target), "sql_maps.html#sql-dest");
    }

    // Body-level refresh is enough for NetBurner CPPCALL pages; head follows
    // in-flight browses on the next request.
    writestring(sock, "<meta http-equiv=\"refresh\" content=\"1;url=");
    writestring(sock, target);
    writestring(sock, "\">");
}

void ShowSqlMapsAutoRefresh(int sock, PCSTR url)
{
    // Only follow an in-flight browse. Catalog autoload runs from ShowSqlMapManager
    // so the 1s refresh cannot re-queue work every second.
    const bool sqlBrowse = SqlRuntimeIsBrowsingDatabases() || SqlRuntimeIsBrowsingTables() ||
                           SqlRuntimeIsBrowsingColumns();
    if (!sqlBrowse) {
        return;
    }
    EmitSqlMapsBrowseRefresh(sock, url);
}

void ShowSqlMapManager(int sock, PCSTR url)
{
    const gateway::GatewayConfig &cfg = gateway::ConfigGet();
    const uint32_t editId = UrlQueryU32(url, "edit");
    const bool wantNew = UrlQueryHasFlag(url, "new");
    const gateway::MappingConfig *edit = nullptr;

    if (!wantNew) {
        if (editId != 0) {
            for (uint16_t i = 0; i < cfg.mapping_count; ++i) {
                if (cfg.mappings[i].id == editId) {
                    edit = &cfg.mappings[i];
                    break;
                }
            }
        } else if (cfg.mapping_count == 1) {
            // Single-mapping workflow: open the only mapping for edit.
            edit = &cfg.mappings[0];
        }
    }

    const bool haveEdit = (edit != nullptr);
    const uint32_t activeEditId = haveEdit ? edit->id : 0;
    const uint32_t preferredBrowsePlc = haveEdit ? edit->plc_id : g_tag_browse_plc_id;
    const bool showForm = (cfg.mapping_count == 0) || haveEdit || wantNew;
    const bool openTagModal = UrlQueryHasFlag(url, "tags");

    char destDb[SQL_CFG_DATABASE_LEN]{};
    char destSchema[32]{};
    char destTable[SQL_CFG_TABLE_LEN]{};
    ResolveMapDestination(edit, destDb, sizeof(destDb), destSchema, sizeof(destSchema), destTable,
                          sizeof(destTable));

    // Catalog loads are started by Refresh Databases / table·column pickers only.
    // Autoload + 1s meta-refresh re-queued SQL browse forever (QUERY RUNNING loop).
    if ((destTable[0] && SqlColumnCatalogMatches(destDb, destTable)) ||
        (SqlCatalogGet().ready && SqlCatalogTablesMatchDatabase(destDb) && !destTable[0])) {
        if (g_status[0] && (strncmp(g_status, "Loading ", 8) == 0)) {
            SetStatus("");
        }
    } else if (SqlRuntimeIsBrowsingDatabases() || SqlRuntimeIsBrowsingTables() ||
               SqlRuntimeIsBrowsingColumns()) {
        EmitSqlMapsBrowseRefresh(sock, url);
    }

    ShowStatusBanner(sock);

    if (cfg.plc_count == 0) {
        writestring(sock, "<p class=\"nb-field-hint\">Add a PLC on the PLCs page before creating SQL "
                          "mappings.</p>");
        return;
    }

    if (cfg.mapping_count > 0) {
        writestring(sock, "<div class=\"nb-card-header\"><span>Mappings</span></div>");
        writestring(sock, "<table class=\"nb-table\"><thead><tr>"
                          "<th>ID</th><th>Name</th><th>PLC</th><th>Op</th><th>Table</th>"
                          "<th>Fields</th><th></th></tr></thead><tbody>");
        for (uint16_t i = 0; i < cfg.mapping_count; ++i) {
            const gateway::MappingConfig &m = cfg.mappings[i];
            const bool rowActive = haveEdit && m.id == activeEditId;
            fdprintf(sock, "<tr%s><td>%lu</td><td>", rowActive ? " class=\"nb-row-active\"" : "",
                     static_cast<unsigned long>(m.id));
            HtmlEsc(sock, m.name);
            fdprintf(sock, "</td><td>%lu</td><td>%s</td><td>", static_cast<unsigned long>(m.plc_id),
                     m.operation == gateway::SqlOperation::Update ? "UPDATE" : "INSERT");
            HtmlEsc(sock, m.table);
            fdprintf(sock, "</td><td>%u</td><td>", static_cast<unsigned>(m.field_count));
            if (!rowActive) {
                fdprintf(sock,
                         "<a class=\"nb-btn nb-btn-secondary nb-btn-inline\" "
                         "href=\"sql_maps.html?edit=%lu\">Edit</a> ",
                         static_cast<unsigned long>(m.id));
            }
            writestring(sock, "<form class=\"nb-inline-form\" method=\"post\" "
                              "action=\"gateway_map_delete\" style=\"display:inline\">");
            fdprintf(sock, "<input type=\"hidden\" name=\"id\" value=\"%lu\">",
                     static_cast<unsigned long>(m.id));
            writestring(sock,
                        "<button type=\"submit\" class=\"nb-btn nb-btn-danger nb-btn-inline\">"
                        "Delete</button></form>");
            writestring(sock, "</td></tr>");
        }
        writestring(sock, "</tbody></table>");

        if (!showForm) {
            writestring(sock, "<p class=\"nb-form-actions\" style=\"margin-top:0.75rem\">"
                              "<a class=\"nb-btn\" href=\"sql_maps.html?new=1\">Add another mapping</a>"
                              "</p>");
            return;
        }

        if (cfg.mapping_count >= 1 && (haveEdit || wantNew)) {
            writestring(sock, "<p class=\"nb-form-actions\" style=\"margin:0.5rem 0 1rem\">");
            if (haveEdit) {
                writestring(sock, "<a class=\"nb-btn nb-btn-secondary\" href=\"sql_maps.html?new=1\">"
                                  "Add another mapping</a> ");
            }
            if (wantNew || haveEdit) {
                writestring(sock, "<a class=\"nb-btn nb-btn-secondary\" href=\"sql_maps.html\">"
                                  "Back to list</a>");
            }
            writestring(sock, "</p>");
        }
    }

    writestring(sock, "<div class=\"nb-card\" style=\"margin-top:1rem\"><div class=\"nb-card-header\"><span>");
    writestring(sock, haveEdit ? "Edit SQL mapping" : "Create SQL mapping");
    writestring(sock, "</span></div>");

    // SQL destination browse (separate POST so catalogs can load without saving the mapping).
    writestring(sock, "<div class=\"nb-card-header\" style=\"margin-top:0.5rem\" id=\"sql-dest\">"
                      "<span>1. SQL destination</span></div>");
    writestring(sock, "<form class=\"nb-form\" method=\"post\" action=\"gateway_map_browse\" "
                      "id=\"nb-sql-dest-form\">");
    fdprintf(sock, "<input type=\"hidden\" name=\"edit\" value=\"%lu\">",
             static_cast<unsigned long>(activeEditId));
    if (wantNew) {
        writestring(sock, "<input type=\"hidden\" name=\"new\" value=\"1\">");
    }
    writestring(sock, "<input type=\"hidden\" name=\"schema\" value=\"dbo\">");
    writestring(sock, "<input type=\"hidden\" name=\"action\" id=\"nb-sql-dest-action\" value=\"\">");
    writestring(sock, "<div class=\"nb-form-grid\">");

    const sql_database_catalog &dbCat = SqlDatabaseCatalogGet();
    writestring(sock, "<label class=\"nb-field\"><span>Database</span>");
    if (dbCat.ready && dbCat.success && dbCat.database_count > 0) {
        WriteNameSelect(sock, "database", destDb, dbCat.database_names, dbCat.database_count, false,
                        "document.getElementById('nb-sql-dest-action').value='tables';"
                        "this.form.submit();");
    } else {
        writestring(sock, "<input type=\"text\" name=\"database\" value=\"");
        HtmlEsc(sock, destDb);
        writestring(sock, "\" placeholder=\"Database name\" "
                          "onchange=\"document.getElementById('nb-sql-dest-action').value='tables';"
                          "this.form.submit();\">");
    }
    writestring(sock, "</label>");

    const sql_table_catalog &tableCat = SqlCatalogGet();
    const bool tablesMatch =
        tableCat.ready && tableCat.success && tableCat.table_count > 0 && destDb[0] &&
        strcmp(tableCat.database, destDb) == 0;
    writestring(sock, "<label class=\"nb-field\"><span>Table</span>");
    if (tablesMatch) {
        WriteNameSelect(sock, "table", destTable, tableCat.table_names, tableCat.table_count, false,
                        "document.getElementById('nb-sql-dest-action').value='columns';"
                        "this.form.submit();");
    } else {
        writestring(sock, "<input type=\"text\" name=\"table\" value=\"");
        HtmlEsc(sock, destTable);
        writestring(sock, "\" placeholder=\"Select database first\" "
                          "onchange=\"document.getElementById('nb-sql-dest-action').value='columns';"
                          "this.form.submit();\">");
    }
    writestring(sock, "</label>");
    writestring(sock, "</div>");
    writestring(sock, "<div class=\"nb-form-actions\">"
                      "<button type=\"button\" class=\"nb-btn nb-btn-secondary\" "
                      "onclick=\"document.getElementById('nb-sql-dest-action').value='databases';"
                      "document.getElementById('nb-sql-dest-form').submit();\">"
                      "Refresh Databases</button></div></form>");
    ShowSqlDestBrowseStatus(sock, destDb, destTable);

    writestring(sock, "<div class=\"nb-card-header\" style=\"margin-top:1rem\" id=\"map-fields\">"
                      "<span>2. Mapping details</span></div>");
    writestring(sock, "<p class=\"nb-form-actions\" style=\"margin:0.5rem 0\">"
                      "<button type=\"button\" class=\"nb-btn nb-btn-secondary\" id=\"nb-open-tag-modal\">"
                      "Browse PLC tags</button></p>");
    writestring(sock, "<form class=\"nb-form\" method=\"post\" action=\"gateway_map_save\">");
    fdprintf(sock, "<input type=\"hidden\" name=\"id\" value=\"%lu\">",
             static_cast<unsigned long>(haveEdit ? edit->id : 0));
    // Carry destination into the save form (browse form is separate).
    writestring(sock, "<input type=\"hidden\" name=\"database\" value=\"");
    HtmlEsc(sock, destDb);
    writestring(sock, "\"><input type=\"hidden\" name=\"schema\" value=\"dbo\">"
                      "<input type=\"hidden\" name=\"table\" value=\"");
    HtmlEsc(sock, destTable);
    writestring(sock, "\">");

    writestring(sock, "<div class=\"nb-form-grid\">");
    writestring(sock, "<label class=\"nb-field\"><span>Name</span><input type=\"text\" name=\"name\" "
                      "required value=\"");
    if (haveEdit) {
        HtmlEsc(sock, edit->name);
    }
    writestring(sock, "\"></label>");

    writestring(sock, "<label class=\"nb-field\"><span>PLC</span><select name=\"plc_id\">");
    for (uint16_t i = 0; i < cfg.plc_count; ++i) {
        const bool sel = haveEdit ? (edit->plc_id == cfg.plcs[i].id)
                                  : (g_tag_browse_plc_id ? cfg.plcs[i].id == g_tag_browse_plc_id : i == 0);
        fdprintf(sock, "<option value=\"%lu\"%s>", static_cast<unsigned long>(cfg.plcs[i].id),
                 sel ? " selected" : "");
        HtmlEsc(sock, cfg.plcs[i].name);
        writestring(sock, "</option>");
    }
    writestring(sock, "</select></label>");

    fdprintf(sock,
             "<label class=\"nb-field\"><span>Enabled</span><select name=\"enabled\">"
             "<option value=\"1\"%s>Yes</option><option value=\"0\"%s>No</option></select></label>",
             (!haveEdit || edit->enabled) ? " selected" : "", (haveEdit && !edit->enabled) ? " selected" : "");
    fdprintf(sock,
             "<label class=\"nb-field\"><span>Operation</span><select name=\"operation\">"
             "<option value=\"0\"%s>INSERT</option><option value=\"1\"%s>UPDATE</option></select></label>",
             (!haveEdit || edit->operation == gateway::SqlOperation::Insert) ? " selected" : "",
             (haveEdit && edit->operation == gateway::SqlOperation::Update) ? " selected" : "");

    writestring(sock, "<label class=\"nb-field\"><span>Trigger tag (BOOL)</span>");
    {
        const char *trig = g_sticky_trigger_set ? g_sticky_trigger : (haveEdit ? edit->trigger_tag : "");
        WriteTagSelect(sock, "trigger_tag", trig, true, true);
    }
    writestring(sock, "</label>");
    writestring(sock, "<label class=\"nb-field\"><span>ACK tag (BOOL)</span>");
    {
        const char *ack = g_sticky_ack_set ? g_sticky_ack : (haveEdit ? edit->ack_tag : "");
        WriteTagSelect(sock, "ack_tag", ack, true, true);
    }
    writestring(sock, "</label>");

    const bool includePlcName = haveEdit && edit->include_plc_name;
    fdprintf(sock,
             "<label class=\"nb-field\"><span>Include PLC name</span><select name=\"include_plc_name\">"
             "<option value=\"0\"%s>No</option><option value=\"1\"%s>Yes</option></select></label>",
             includePlcName ? "" : " selected", includePlcName ? " selected" : "");
    writestring(sock, "<label class=\"nb-field\"><span>PLC name SQL column</span>");
    WriteNamedColumnSelect(sock, "plc_name_column",
                           (haveEdit && edit->plc_name_column[0]) ? edit->plc_name_column : "", destDb,
                           destTable, false);
    writestring(sock, "</label>");
    writestring(sock, "<p class=\"nb-field-hint\" style=\"grid-column:1/-1\">Optional: write the configured PLC "
                      "common name (from PLCs page) into a SQL column on each event — no tag read.</p>");

    writestring(sock, "<label class=\"nb-field\"><span>Database</span><input type=\"text\" "
                      "name=\"database_display\" disabled value=\"");
    HtmlEsc(sock, destDb);
    writestring(sock, "\"></label>");
    writestring(sock, "<label class=\"nb-field\"><span>Table</span><input type=\"text\" "
                      "name=\"table_display\" disabled value=\"");
    HtmlEsc(sock, destTable);
    writestring(sock, "\"></label>");
    writestring(sock, "</div>");

    const bool isUpdate =
        haveEdit && edit->operation == gateway::SqlOperation::Update;
    writestring(sock, "<p class=\"nb-field-hint\">3. Field rows — use <strong>Browse PLC tags</strong> to fill "
                      "trigger/ACK/field tags, then pick SQL columns from the loaded catalog (up to 8).");
    if (isUpdate) {
        writestring(sock, " Mark at least one column as a <strong>WHERE</strong> key for UPDATE.</p>");
    } else {
        writestring(sock, "</p>");
    }

    const bool haveColumnCatalog = ColumnsCatalogReady(destDb, destTable);
    const bool havePlcBrowseTypes = TagBrowseReady();
    writestring(sock, "<table class=\"nb-table\"><thead><tr>"
                      "<th>PLC tag</th><th>PLC type</th>");
    writestring(sock, "<th>SQL column</th>");
    if (!haveColumnCatalog) {
        writestring(sock, "<th>SQL type</th>");
    }
    if (isUpdate) {
        writestring(sock, "<th>WHERE key</th>");
    }
    writestring(sock, "</tr></thead><tbody>");

    for (uint16_t r = 0; r < 8; ++r) {
        const bool has = haveEdit && r < edit->field_count;
        const gateway::FieldMapping *f = has ? &edit->fields[r] : nullptr;
        const bool useSticky = g_sticky_fset[r];
        int ptype = useSticky ? g_sticky_fptype[r] : (f ? static_cast<int>(f->plc_type) : -1);
        const char *tagSel = nullptr;
        if (useSticky && g_sticky_ftag[r][0]) {
            tagSel = g_sticky_ftag[r];
        } else if (f && f->plc_tag[0]) {
            tagSel = f->plc_tag;
        }
        // Prefer live browse CIP type when available so USINT/UDINT are not shown as SINT/DINT.
        if (tagSel && tagSel[0]) {
            int fromBrowse = -1;
            if (LookupPlcTypeFromBrowse(tagSel, &fromBrowse)) {
                ptype = fromBrowse;
            }
        }
        const char *colSel = (f && f->sql_column[0]) ? f->sql_column : nullptr;

        writestring(sock, "<tr>");
        writestring(sock, "<td>");
        char ftagName[16]{};
        sniprintf(ftagName, sizeof(ftagName), "ftag%u", static_cast<unsigned>(r));
        WriteTagSelect(sock, ftagName, tagSel, false, false);
        writestring(sock, "</td>");
        {
            const int selectType = (ptype >= 0 && ptype != static_cast<int>(gateway::PlcDataType::Unknown))
                                       ? ptype
                                       : static_cast<int>(gateway::PlcDataType::Dint);
            fdprintf(sock, "<td><select name=\"fptype%u\">", static_cast<unsigned>(r));
            WriteTypeOptions(sock, selectType, true);
            writestring(sock, "</select>");
            if (havePlcBrowseTypes && ptype >= 0 &&
                ptype != static_cast<int>(gateway::PlcDataType::Unknown)) {
                fdprintf(sock, "<div class=\"nb-field-hint\">browse: %s</div>", PlcTypeName(ptype));
            }
            writestring(sock, "</td>");
        }
        writestring(sock, "<td>");
        WriteColumnSelect(sock, r, colSel, destDb, destTable);
        writestring(sock, "</td>");
        if (!haveColumnCatalog) {
            const int sqlType = f ? static_cast<int>(f->sql_type) : 3;
            fdprintf(sock, "<td><select name=\"fstype%u\">", static_cast<unsigned>(r));
            WriteTypeOptions(sock, sqlType, false);
            writestring(sock, "</select></td>");
        }
        if (isUpdate) {
            fdprintf(sock, "<td><input type=\"checkbox\" name=\"fkey%u\" value=\"1\"%s></td>",
                     static_cast<unsigned>(r), (f && f->is_where_key) ? " checked" : "");
        }
        writestring(sock, "</tr>");
    }
    writestring(sock, "</tbody></table>");
    writestring(sock, "<div class=\"nb-form-actions\">"
                      "<button type=\"submit\" class=\"nb-btn\">Save Mapping</button></div></form>");

    if (haveEdit) {
        char sql[SQL_CFG_QUERY_LEN]{};
        char err[96]{};
        writestring(sock, "<div class=\"nb-card-header\" style=\"margin-top:1rem\"><span>SQL preview</span></div>");
        if (gateway::BuildSqlPreviewFromMapping(*edit, sql, sizeof(sql), err, sizeof(err))) {
            writestring(sock, "<p class=\"nb-field-hint\">Sample values only — live inserts use current PLC tag reads.</p>");
            writestring(sock, "<pre class=\"nb-code\">");
            HtmlEsc(sock, sql);
            writestring(sock, "</pre>");
        } else {
            writestring(sock, "<p class=\"nb-field-hint\">Preview failed: ");
            HtmlEsc(sock, err);
            writestring(sock, "</p>");
        }
    }

    ShowTagBrowser(sock, cfg, preferredBrowsePlc, activeEditId, wantNew, openTagModal);
    writestring(sock, "</div>");
}

static void EnablePostCallback(int sock, PostEvents event, const char *pName, const char *pValue)
{
    switch (event) {
    case eStartingPost:
        g_post_enable = false;
        break;
    case eVariable:
        if (pName && strcmp(pName, "enabled") == 0) {
            g_post_enable = (pValue && pValue[0] == '1');
        }
        break;
    case eEndOfPost: {
        gateway::GatewayConfig &cfg = gateway::ConfigMutable();
        cfg.enabled = g_post_enable;
        char err[96]{};
        if (PersistConfig(cfg, err, sizeof(err))) {
            SetStatus(cfg.enabled ? "Gateway enabled." : "Gateway disabled.");
        } else {
            SetStatus(err[0] ? err : "Failed to save enable flag.");
        }
        RedirectResponse(sock, "gateway.html");
        break;
    }
    default:
        break;
    }
}

static void PlcSavePostCallback(int sock, PostEvents event, const char *pName, const char *pValue)
{
    switch (event) {
    case eStartingPost:
        g_post_plc = gateway::PlcConfig{};
        g_post_plc.enabled = true;
        g_post_plc.poll_interval_ms = 200;
        g_post_plc.request_timeout_ms = 3000;
        g_post_plc.retry_delay_ms = 500;
        break;
    case eVariable:
        if (!pName) {
            break;
        }
        if (strcmp(pName, "id") == 0) {
            g_post_plc.id = ParseU32(pValue);
        } else if (strcmp(pName, "name") == 0) {
            CopyField(g_post_plc.name, sizeof(g_post_plc.name), pValue);
        } else if (strcmp(pName, "ip") == 0) {
            ParseIpv4(pValue, g_post_plc.ip);
        } else if (strcmp(pName, "enabled") == 0) {
            g_post_plc.enabled = (pValue && pValue[0] == '1');
        } else if (strcmp(pName, "poll_ms") == 0) {
            g_post_plc.poll_interval_ms = ParseU32(pValue, 200);
        } else if (strcmp(pName, "timeout_ms") == 0) {
            g_post_plc.request_timeout_ms = ParseU32(pValue, 3000);
        } else if (strcmp(pName, "retry_ms") == 0) {
            g_post_plc.retry_delay_ms = ParseU32(pValue, 500);
        }
        break;
    case eEndOfPost: {
        gateway::GatewayConfig &cfg = gateway::ConfigMutable();
        char err[96]{};
        if (!g_post_plc.name[0] || static_cast<uint32_t>(g_post_plc.ip) == 0) {
            SetStatus("Name and IP are required.");
            RedirectResponse(sock, "plc.html");
            break;
        }
        if (g_post_plc.id == 0) {
            if (cfg.plc_count >= gateway::kMaxPlcs) {
                SetStatus("PLC limit reached.");
                RedirectResponse(sock, "plc.html");
                break;
            }
            g_post_plc.id = NextPlcId(cfg);
            cfg.plcs[cfg.plc_count++] = g_post_plc;
        } else {
            bool found = false;
            for (uint16_t i = 0; i < cfg.plc_count; ++i) {
                if (cfg.plcs[i].id == g_post_plc.id) {
                    cfg.plcs[i] = g_post_plc;
                    found = true;
                    break;
                }
            }
            if (!found) {
                SetStatus("PLC not found.");
                RedirectResponse(sock, "plc.html");
                break;
            }
        }
        if (PersistConfig(cfg, err, sizeof(err))) {
            SetStatus("PLC saved.");
            g_edit_plc_id = 0;
        } else {
            SetStatus(err[0] ? err : "Failed to save PLC.");
        }
        RedirectResponse(sock, "plc.html");
        break;
    }
    default:
        break;
    }
}

static void PlcDeletePostCallback(int sock, PostEvents event, const char *pName, const char *pValue)
{
    switch (event) {
    case eStartingPost:
        g_post_delete_id = 0;
        break;
    case eVariable:
        if (pName && strcmp(pName, "id") == 0) {
            g_post_delete_id = ParseU32(pValue);
        }
        break;
    case eEndOfPost: {
        gateway::GatewayConfig &cfg = gateway::ConfigMutable();
        char err[96]{};
        bool removed = false;
        for (uint16_t i = 0; i < cfg.plc_count; ++i) {
            if (cfg.plcs[i].id == g_post_delete_id) {
                for (uint16_t j = i + 1; j < cfg.plc_count; ++j) {
                    cfg.plcs[j - 1] = cfg.plcs[j];
                }
                --cfg.plc_count;
                removed = true;
                break;
            }
        }
        // Drop mappings that referenced the PLC
        uint16_t w = 0;
        for (uint16_t i = 0; i < cfg.mapping_count; ++i) {
            if (cfg.mappings[i].plc_id != g_post_delete_id) {
                cfg.mappings[w++] = cfg.mappings[i];
            }
        }
        cfg.mapping_count = w;

        if (!removed) {
            SetStatus("PLC not found.");
        } else if (PersistConfig(cfg, err, sizeof(err))) {
            SetStatus("PLC deleted.");
        } else {
            SetStatus(err[0] ? err : "Failed to delete PLC.");
        }
        RedirectResponse(sock, "plc.html");
        break;
    }
    default:
        break;
    }
}

static void PlcScanPostCallback(int sock, PostEvents event, const char * /*pName*/, const char * /*pValue*/)
{
    if (event != eEndOfPost) {
        return;
    }

    g_scan_err[0] = '\0';
    g_scan_count = 0;
    for (int i = 0; i < micro800::kMaxDiscoveredDevices; ++i) {
        g_scan_devices[i] = micro800::DiscoveredDevice{};
    }

    g_scan_count = micro800::ScanListIdentity(g_scan_devices, micro800::kMaxDiscoveredDevices, g_scan_err,
                                              sizeof(g_scan_err));
    g_scan_ran = true;

    if (g_scan_count < 0) {
        g_scan_count = 0;
    }

    int micro800Count = 0;
    for (int i = 0; i < g_scan_count; ++i) {
        if (g_scan_devices[i].valid && micro800::LooksLikeMicro800(g_scan_devices[i])) {
            ++micro800Count;
        }
    }

    char msg[120]{};
    if (g_scan_err[0] && g_scan_count == 0) {
        sniprintf(msg, sizeof(msg), "Scan failed: %s", g_scan_err);
    } else {
        sniprintf(msg, sizeof(msg), "Scan complete: %d device(s), %d Micro800 candidate(s).", g_scan_count,
                  micro800Count);
    }
    SetStatus(msg);
    RedirectResponse(sock, "plc.html");
}

static void PlcAddFromScanPostCallback(int sock, PostEvents event, const char *pName, const char *pValue)
{
    switch (event) {
    case eStartingPost:
        g_post_scan_ip = IPADDR4{};
        g_post_scan_name[0] = '\0';
        break;
    case eVariable:
        if (!pName) {
            break;
        }
        if (strcmp(pName, "ip") == 0) {
            ParseIpv4(pValue, g_post_scan_ip);
        } else if (strcmp(pName, "name") == 0) {
            CopyField(g_post_scan_name, sizeof(g_post_scan_name), pValue);
        }
        break;
    case eEndOfPost: {
        gateway::GatewayConfig &cfg = gateway::ConfigMutable();
        char err[96]{};
        if (static_cast<uint32_t>(g_post_scan_ip) == 0) {
            SetStatus("Scan add requires a valid IP.");
            RedirectResponse(sock, "plc.html");
            break;
        }
        if (PlcIpConfigured(cfg, g_post_scan_ip)) {
            SetStatus("That PLC IP is already configured.");
            RedirectResponse(sock, "plc.html");
            break;
        }
        if (cfg.plc_count >= gateway::kMaxPlcs) {
            SetStatus("PLC limit reached.");
            RedirectResponse(sock, "plc.html");
            break;
        }

        gateway::PlcConfig plc{};
        plc.id = NextPlcId(cfg);
        plc.enabled = true;
        plc.ip = g_post_scan_ip;
        plc.poll_interval_ms = 200;
        plc.request_timeout_ms = 3000;
        plc.retry_delay_ms = 500;
        if (g_post_scan_name[0]) {
            CopyField(plc.name, sizeof(plc.name), g_post_scan_name);
        } else {
            FormatIpv4(plc.name, sizeof(plc.name), g_post_scan_ip);
        }

        cfg.plcs[cfg.plc_count++] = plc;
        if (PersistConfig(cfg, err, sizeof(err))) {
            SetStatus("PLC added from scan.");
        } else {
            SetStatus(err[0] ? err : "Failed to save scanned PLC.");
        }
        RedirectResponse(sock, "plc.html");
        break;
    }
    default:
        break;
    }
}

static void MapBrowsePostCallback(int sock, PostEvents event, const char *pName, const char *pValue)
{
    switch (event) {
    case eStartingPost:
        g_post_map_db[0] = '\0';
        g_post_map_table[0] = '\0';
        g_post_map_schema[0] = '\0';
        g_post_sql_browse_action[0] = '\0';
        g_post_return_edit = 0;
        g_post_return_new = false;
        break;
    case eVariable:
        if (!pName) {
            break;
        }
        if (strcmp(pName, "database") == 0) {
            CopyField(g_post_map_db, sizeof(g_post_map_db), pValue);
        } else if (strcmp(pName, "table") == 0) {
            CopyField(g_post_map_table, sizeof(g_post_map_table), pValue);
        } else if (strcmp(pName, "schema") == 0) {
            CopyField(g_post_map_schema, sizeof(g_post_map_schema), pValue);
        } else if (strcmp(pName, "action") == 0) {
            CopyField(g_post_sql_browse_action, sizeof(g_post_sql_browse_action), pValue);
        } else if (strcmp(pName, "edit") == 0) {
            g_post_return_edit = ParseU32(pValue);
        } else if (strcmp(pName, "new") == 0) {
            g_post_return_new = (pValue && pValue[0] == '1');
        }
        break;
    case eEndOfPost: {
        // Changing database invalidates the previous table selection.
        if (g_sticky_dest_set && g_sticky_database[0] && g_post_map_db[0] &&
            strcmp(g_sticky_database, g_post_map_db) != 0) {
            g_post_map_table[0] = '\0';
        }

        CopyField(g_sticky_database, sizeof(g_sticky_database), g_post_map_db);
        CopyField(g_sticky_table, sizeof(g_sticky_table), g_post_map_table);
        CopyField(g_sticky_schema, sizeof(g_sticky_schema), "dbo");
        g_sticky_dest_set = true;

        if (g_post_return_edit) {
            g_edit_map_id = g_post_return_edit;
        }

        if (SqlRuntimeIsBusy()) {
            SetStatus("SQL Server is busy — try again in a moment.");
            RedirectSqlMaps(sock, g_post_return_edit, g_post_return_new, "sql-dest");
            break;
        }

        sql_runtime_config sqlCfg = SqlRuntimeGetConfig();
        if (g_post_map_db[0]) {
            CopyField(sqlCfg.database, sizeof(sqlCfg.database), g_post_map_db);
        }
        CopyField(sqlCfg.table, sizeof(sqlCfg.table), g_post_map_table);
        SqlRuntimeUpdateConfig(sqlCfg);

        if (strcmp(g_post_sql_browse_action, "databases") == 0) {
            SqlRuntimeRequestBrowseDatabases();
            SetStatus("Loading databases...");
        } else if (strcmp(g_post_sql_browse_action, "tables") == 0) {
            if (!g_post_map_db[0]) {
                SetStatus("Pick a database before loading tables.");
            } else {
                SqlRuntimeRequestBrowseTables();
                SetStatus("Loading tables...");
            }
        } else if (strcmp(g_post_sql_browse_action, "columns") == 0) {
            if (!g_post_map_db[0] || !g_post_map_table[0]) {
                SetStatus("Pick a database and table before loading columns.");
            } else {
                SqlRuntimeRequestBrowseColumns();
                SetStatus("Loading columns...");
            }
        } else if (g_post_map_db[0]) {
            // Default: selecting a database without an explicit action still loads tables.
            SqlRuntimeRequestBrowseTables();
            SetStatus("Loading tables...");
        } else {
            SetStatus("Pick a database.");
        }

        RedirectSqlMaps(sock, g_post_return_edit, g_post_return_new, "sql-dest");
        break;
    }
    default:
        break;
    }
}

static void MapSavePostCallback(int sock, PostEvents event, const char *pName, const char *pValue)
{
    switch (event) {
    case eStartingPost:
        g_post_map = gateway::MappingConfig{};
        g_post_map.enabled = true;
        g_post_map.operation = gateway::SqlOperation::Insert;
        CopyField(g_post_map.schema, sizeof(g_post_map.schema), "dbo");
        memset(g_post_freq, 0, sizeof(g_post_freq));
        memset(g_post_fkey, 0, sizeof(g_post_fkey));
        memset(g_post_ftag, 0, sizeof(g_post_ftag));
        memset(g_post_fcol, 0, sizeof(g_post_fcol));
        for (int i = 0; i < 8; ++i) {
            g_post_fptype[i] = static_cast<int>(gateway::PlcDataType::Unknown);
            g_post_fstype[i] = static_cast<int>(gateway::SqlDataType::Unknown);
        }
        break;
    case eVariable: {
        if (!pName) {
            break;
        }
        if (strcmp(pName, "id") == 0) {
            g_post_map.id = ParseU32(pValue);
        } else if (strcmp(pName, "name") == 0) {
            CopyField(g_post_map.name, sizeof(g_post_map.name), pValue);
        } else if (strcmp(pName, "plc_id") == 0) {
            g_post_map.plc_id = ParseU32(pValue);
        } else if (strcmp(pName, "enabled") == 0) {
            g_post_map.enabled = (pValue && pValue[0] == '1');
        } else if (strcmp(pName, "operation") == 0) {
            g_post_map.operation =
                (pValue && pValue[0] == '1') ? gateway::SqlOperation::Update : gateway::SqlOperation::Insert;
        } else if (strcmp(pName, "trigger_tag") == 0) {
            CopyField(g_post_map.trigger_tag, sizeof(g_post_map.trigger_tag), pValue);
        } else if (strcmp(pName, "ack_tag") == 0) {
            CopyField(g_post_map.ack_tag, sizeof(g_post_map.ack_tag), pValue);
        } else if (strcmp(pName, "database") == 0) {
            CopyField(g_post_map.database, sizeof(g_post_map.database), pValue);
        } else if (strcmp(pName, "schema") == 0) {
            CopyField(g_post_map.schema, sizeof(g_post_map.schema), pValue);
        } else if (strcmp(pName, "table") == 0) {
            CopyField(g_post_map.table, sizeof(g_post_map.table), pValue);
        } else if (strcmp(pName, "include_plc_name") == 0) {
            g_post_map.include_plc_name = (pValue && pValue[0] == '1');
        } else if (strcmp(pName, "plc_name_column") == 0) {
            CopyField(g_post_map.plc_name_column, sizeof(g_post_map.plc_name_column), pValue);
        } else if (strncmp(pName, "ftag", 4) == 0) {
            const unsigned idx = ParseU32(pName + 4);
            if (idx < 8) {
                CopyField(g_post_ftag[idx], sizeof(g_post_ftag[idx]), pValue);
            }
        } else if (strncmp(pName, "fcol", 4) == 0) {
            const unsigned idx = ParseU32(pName + 4);
            if (idx < 8) {
                CopyField(g_post_fcol[idx], sizeof(g_post_fcol[idx]), pValue);
            }
        } else if (strncmp(pName, "fptype", 6) == 0) {
            const unsigned idx = ParseU32(pName + 6);
            if (idx < 8) {
                g_post_fptype[idx] = atoi(pValue ? pValue : "3");
            }
        } else if (strncmp(pName, "fstype", 6) == 0) {
            const unsigned idx = ParseU32(pName + 6);
            if (idx < 8) {
                g_post_fstype[idx] = atoi(pValue ? pValue : "3");
            }
        } else if (strncmp(pName, "freq", 4) == 0) {
            const unsigned idx = ParseU32(pName + 4);
            if (idx < 8) {
                g_post_freq[idx] = true;
            }
        } else if (strncmp(pName, "fkey", 4) == 0) {
            const unsigned idx = ParseU32(pName + 4);
            if (idx < 8) {
                g_post_fkey[idx] = true;
            }
        }
        break;
    }
    case eEndOfPost: {
        g_post_map.field_count = 0;
        bool abortSave = false;
        for (uint16_t i = 0; i < 8 && g_post_map.field_count < gateway::kMaxFieldsPerMapping; ++i) {
            if (!g_post_ftag[i][0] || !g_post_fcol[i][0]) {
                continue;
            }
            char colNorm[gateway::kMaxSqlIdentLen]{};
            CopyField(colNorm, sizeof(colNorm), g_post_fcol[i]);
            NormalizeSqlIdentField(colNorm);
            bool dupCol = false;
            for (uint16_t j = 0; j < g_post_map.field_count; ++j) {
                if (strcmp(g_post_map.fields[j].sql_column, colNorm) == 0) {
                    dupCol = true;
                    break;
                }
            }
            if (dupCol) {
                SetStatus("Duplicate SQL column in field rows — each column once.");
                abortSave = true;
                break;
            }
            gateway::FieldMapping &f = g_post_map.fields[g_post_map.field_count++];
            f = gateway::FieldMapping{};
            CopyField(f.plc_tag, sizeof(f.plc_tag), g_post_ftag[i]);
            CopyField(f.sql_column, sizeof(f.sql_column), colNorm);

            int ptype = g_post_fptype[i];
            // Honor the posted PLC type dropdown. Only fall back to browse/sticky when
            // the form did not supply a concrete type (legacy / missing field).
            if (ptype < 0 || ptype == static_cast<int>(gateway::PlcDataType::Unknown)) {
                int fromBrowse = 0;
                if (LookupPlcTypeFromBrowse(f.plc_tag, &fromBrowse)) {
                    ptype = fromBrowse;
                } else {
                    for (int r = 0; r < 8; ++r) {
                        if (g_sticky_fset[r] && strcmp(g_sticky_ftag[r], f.plc_tag) == 0) {
                            ptype = g_sticky_fptype[r];
                            break;
                        }
                    }
                }
            }
            if (ptype < 0 || ptype == static_cast<int>(gateway::PlcDataType::Unknown)) {
                ptype = static_cast<int>(gateway::PlcDataType::Dint);
            }
            f.plc_type = static_cast<gateway::PlcDataType>(ptype);

            int sqlType = g_post_fstype[i];
            int fromCatalog = 0;
            if (LookupCatalogSqlType(g_post_map.database, g_post_map.table, f.sql_column, &fromCatalog)) {
                sqlType = fromCatalog;
            } else if (sqlType == static_cast<int>(gateway::SqlDataType::Unknown)) {
                sqlType = static_cast<int>(gateway::SqlDataType::NVarChar);
            }
            f.sql_type = static_cast<gateway::SqlDataType>(sqlType);
            // Mapped fields always require a successful PLC read (no per-row Req UI).
            f.required = true;
            f.nullable = false;
            f.is_where_key = g_post_fkey[i];
            // WHERE keys only apply to UPDATE; ignore stray checkboxes on INSERT.
            if (g_post_map.operation != gateway::SqlOperation::Update) {
                f.is_where_key = false;
            }
        }
        if (abortSave) {
            char redir[64]{};
            if (g_post_map.id) {
                sniprintf(redir, sizeof(redir), "sql_maps.html?edit=%lu",
                          static_cast<unsigned long>(g_post_map.id));
            } else {
                CopyField(redir, sizeof(redir), "sql_maps.html?new=1");
            }
            RedirectResponse(sock, redir);
            break;
        }
        NormalizeSqlIdentField(g_post_map.database);
        NormalizeSqlIdentField(g_post_map.table);
        NormalizeSqlIdentField(g_post_map.plc_name_column);
        CopyField(g_post_map.schema, sizeof(g_post_map.schema), "dbo");
        if (!g_post_map.include_plc_name) {
            g_post_map.plc_name_column[0] = '\0';
        }

        gateway::GatewayConfig &cfg = gateway::ConfigMutable();
        char err[96]{};
        if (g_post_map.id == 0) {
            if (cfg.mapping_count >= gateway::kMaxMappings) {
                SetStatus("Mapping limit reached.");
                RedirectResponse(sock, "sql_maps.html");
                break;
            }
            g_post_map.id = NextMapId(cfg);
            cfg.mappings[cfg.mapping_count++] = g_post_map;
        } else {
            bool found = false;
            for (uint16_t i = 0; i < cfg.mapping_count; ++i) {
                if (cfg.mappings[i].id == g_post_map.id) {
                    cfg.mappings[i] = g_post_map;
                    found = true;
                    break;
                }
            }
            if (!found) {
                SetStatus("Mapping not found.");
                RedirectResponse(sock, "sql_maps.html");
                break;
            }
        }
        if (PersistConfig(cfg, err, sizeof(err))) {
            SetStatus("Mapping saved.");
            ClearTagStickies();
            g_edit_map_id = g_post_map.id;
            char redir[48];
            sniprintf(redir, sizeof(redir), "sql_maps.html?edit=%lu", static_cast<unsigned long>(g_post_map.id));
            RedirectResponse(sock, redir);
        } else {
            SetStatus(err[0] ? err : "Failed to save mapping.");
            RedirectResponse(sock, "sql_maps.html");
        }
        break;
    }
    default:
        break;
    }
}

static void MapDeletePostCallback(int sock, PostEvents event, const char *pName, const char *pValue)
{
    switch (event) {
    case eStartingPost:
        g_post_delete_id = 0;
        break;
    case eVariable:
        if (pName && strcmp(pName, "id") == 0) {
            g_post_delete_id = ParseU32(pValue);
        }
        break;
    case eEndOfPost: {
        gateway::GatewayConfig &cfg = gateway::ConfigMutable();
        char err[96]{};
        bool removed = false;
        for (uint16_t i = 0; i < cfg.mapping_count; ++i) {
            if (cfg.mappings[i].id == g_post_delete_id) {
                for (uint16_t j = i + 1; j < cfg.mapping_count; ++j) {
                    cfg.mappings[j - 1] = cfg.mappings[j];
                }
                --cfg.mapping_count;
                removed = true;
                break;
            }
        }
        if (!removed) {
            SetStatus("Mapping not found.");
        } else if (PersistConfig(cfg, err, sizeof(err))) {
            SetStatus("Mapping deleted.");
            g_edit_map_id = 0;
        } else {
            SetStatus(err[0] ? err : "Failed to delete mapping.");
        }
        RedirectResponse(sock, "sql_maps.html");
        break;
    }
    default:
        break;
    }
}

static void TagBrowsePostCallback(int sock, PostEvents event, const char *pName, const char *pValue)
{
    switch (event) {
    case eStartingPost:
        g_post_browse_plc_id = 0;
        g_post_return_edit = 0;
        g_post_return_new = false;
        break;
    case eVariable:
        if (!pName) {
            break;
        }
        if (strcmp(pName, "plc_id") == 0) {
            g_post_browse_plc_id = ParseU32(pValue);
        } else if (strcmp(pName, "edit") == 0) {
            g_post_return_edit = ParseU32(pValue);
        } else if (strcmp(pName, "new") == 0) {
            g_post_return_new = (pValue && pValue[0] == '1');
        }
        break;
    case eEndOfPost: {
        const gateway::GatewayConfig &cfg = gateway::ConfigGet();
        gateway::PlcConfig plc{};
        g_tag_browse_err[0] = '\0';
        g_tag_count = 0;
        g_tag_browse_ran = true;
        g_tag_browse_plc_id = g_post_browse_plc_id;

        if (!gateway::ConfigFindPlc(g_post_browse_plc_id, plc)) {
            SetStatus("Select a configured PLC before browsing tags.");
            RedirectSqlMaps(sock, g_post_return_edit, g_post_return_new, nullptr, true);
            break;
        }

        for (int i = 0; i < kMaxWebBrowseTags; ++i) {
            g_tags[i] = micro800::BrowsedTag{};
        }

        g_tag_count = micro800::BrowseTags(plc.ip, g_tags, kMaxWebBrowseTags, g_tag_browse_err,
                                           sizeof(g_tag_browse_err));
        if (g_tag_count < 0) {
            g_tag_count = 0;
        }

        char msg[120]{};
        if (g_tag_browse_err[0] && g_tag_count == 0) {
            sniprintf(msg, sizeof(msg), "Tag browse failed: %s", g_tag_browse_err);
        } else {
            sniprintf(msg, sizeof(msg), "Tag browse: %d tag(s) from PLC %lu.", g_tag_count,
                      static_cast<unsigned long>(plc.id));
        }
        SetStatus(msg);
        if (g_post_return_edit) {
            g_edit_map_id = g_post_return_edit;
        }
        RedirectSqlMaps(sock, g_post_return_edit, g_post_return_new, nullptr, true);
        break;
    }
    default:
        break;
    }
}

static void TagUsePostCallback(int sock, PostEvents event, const char *pName, const char *pValue)
{
    switch (event) {
    case eStartingPost:
        g_post_use_tag[0] = '\0';
        g_post_use_symbol = 0;
        g_post_use_action[0] = '\0';
        g_post_return_edit = 0;
        g_post_return_new = false;
        break;
    case eVariable:
        if (!pName) {
            break;
        }
        if (strcmp(pName, "tag") == 0) {
            CopyField(g_post_use_tag, sizeof(g_post_use_tag), pValue);
        } else if (strcmp(pName, "symbol") == 0) {
            g_post_use_symbol = static_cast<uint16_t>(ParseU32(pValue));
        } else if (strcmp(pName, "action") == 0) {
            CopyField(g_post_use_action, sizeof(g_post_use_action), pValue);
        } else if (strcmp(pName, "edit") == 0) {
            g_post_return_edit = ParseU32(pValue);
        } else if (strcmp(pName, "new") == 0) {
            g_post_return_new = (pValue && pValue[0] == '1');
        }
        break;
    case eEndOfPost: {
        if (!g_post_use_tag[0] || !g_post_use_action[0]) {
            SetStatus("Tag use requires tag and action.");
            RedirectSqlMaps(sock, g_post_return_edit, g_post_return_new, nullptr, true);
            break;
        }

        if (strcmp(g_post_use_action, "trigger") == 0) {
            CopyField(g_sticky_trigger, sizeof(g_sticky_trigger), g_post_use_tag);
            g_sticky_trigger_set = true;
            SetStatus("Trigger tag filled from browse.");
        } else if (strcmp(g_post_use_action, "ack") == 0) {
            CopyField(g_sticky_ack, sizeof(g_sticky_ack), g_post_use_tag);
            g_sticky_ack_set = true;
            SetStatus("ACK tag filled from browse.");
        } else if (strcmp(g_post_use_action, "field") == 0) {
            const gateway::GatewayConfig &cfg = gateway::ConfigGet();
            const gateway::MappingConfig *edit = nullptr;
            for (uint16_t i = 0; i < cfg.mapping_count; ++i) {
                if (cfg.mappings[i].id == g_post_return_edit) {
                    edit = &cfg.mappings[i];
                    break;
                }
            }

            int slot = -1;
            for (int r = 0; r < 8; ++r) {
                // Sticky and saved mapping rows both occupy a slot (OR, not else-if).
                const bool stickyOcc = g_sticky_fset[r] && g_sticky_ftag[r][0];
                const bool savedOcc =
                    edit && r < static_cast<int>(edit->field_count) && edit->fields[r].plc_tag[0];
                if (!stickyOcc && !savedOcc) {
                    slot = r;
                    break;
                }
            }
            if (slot < 0) {
                SetStatus("All 8 field rows are filled.");
            } else {
                CopyField(g_sticky_ftag[slot], sizeof(g_sticky_ftag[slot]), g_post_use_tag);
                g_sticky_fptype[slot] = PlcTypeOptionFromSymbol(g_post_use_symbol);
                g_sticky_fset[slot] = true;
                SetStatus("Field row filled from browse.");
            }
        } else {
            SetStatus("Unknown tag use action.");
        }

        if (g_post_return_edit) {
            g_edit_map_id = g_post_return_edit;
        }
        RedirectSqlMaps(sock, g_post_return_edit, g_post_return_new, nullptr, true);
        break;
    }
    default:
        break;
    }
}

HtmlPostVariableListCallback g_gateway_enable_post("gateway_enable*", EnablePostCallback);
HtmlPostVariableListCallback g_gateway_plc_save_post("gateway_plc_save*", PlcSavePostCallback);
HtmlPostVariableListCallback g_gateway_plc_delete_post("gateway_plc_delete*", PlcDeletePostCallback);
HtmlPostVariableListCallback g_gateway_plc_scan_post("gateway_plc_scan*", PlcScanPostCallback);
HtmlPostVariableListCallback g_gateway_plc_add_scan_post("gateway_plc_add_scan*", PlcAddFromScanPostCallback);
HtmlPostVariableListCallback g_gateway_map_save_post("gateway_map_save*", MapSavePostCallback);
HtmlPostVariableListCallback g_gateway_map_delete_post("gateway_map_delete*", MapDeletePostCallback);
HtmlPostVariableListCallback g_gateway_map_browse_post("gateway_map_browse*", MapBrowsePostCallback);
HtmlPostVariableListCallback g_gateway_tag_browse_post("gateway_tag_browse*", TagBrowsePostCallback);
HtmlPostVariableListCallback g_gateway_tag_use_post("gateway_tag_use*", TagUsePostCallback);

#else

#endif
