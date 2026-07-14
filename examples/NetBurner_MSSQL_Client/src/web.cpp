// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>

#include <fdprintf.h>
#include <httppost.h>
#include <http.h>
#include <nbrtos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sql_catalog.h"
#include "sql_nv.h"
#include "sql_results.h"
#include "sql_runtime.h"

static sql_runtime_config g_post_config = {};
static bool g_post_use_table_builder    = false;
static bool g_post_password_provided    = false;
static bool g_post_columns_seen         = false;
static char g_post_return_to[64]        = {};

static char g_write_sql[SQL_CFG_QUERY_LEN] = {};
static char g_write_where_column[SQL_CFG_TABLE_LEN] = {};
static char g_write_where_value[SQL_CFG_FILTER_LEN] = {};
static char g_write_cols[SQL_CATALOG_MAX_COLUMNS][SQL_CATALOG_NAME_LEN] = {};
static char g_write_vals[SQL_CATALOG_MAX_COLUMNS][SQL_CFG_FILTER_LEN] = {};
static bool g_write_use[SQL_CATALOG_MAX_COLUMNS] = {};

static void HtmlWriteEscaped(int sock, const char *text)
{
    if (text == nullptr) {
        return;
    }

    for (const char *p = text; *p != '\0'; ++p) {
        switch (*p) {
            case '&':
                fdprintf(sock, "&amp;");
                break;
            case '<':
                fdprintf(sock, "&lt;");
                break;
            case '>':
                fdprintf(sock, "&gt;");
                break;
            case '"':
                fdprintf(sock, "&quot;");
                break;
            default:
                fdprintf(sock, "%c", *p);
                break;
        }
    }
}

static void CsvWriteEscaped(int sock, const char * text)
{
    if (text == nullptr) {
        return;
    }

    bool quote = false;
    for (const char * p = text; *p != '\0'; ++p) {
        if (*p == ',' || *p == '"' || *p == '\r' || *p == '\n') {
            quote = true;
            break;
        }
    }

    if (quote) {
        fdprintf(sock, "\"");
    }

    for (const char * p = text; *p != '\0'; ++p) {
        if (*p == '"') {
            fdprintf(sock, "\"\"");
        }
        else {
            fdprintf(sock, "%c", *p);
        }
    }

    if (quote) {
        fdprintf(sock, "\"");
    }
}

static void CopyField(char *dst, tdsl::size_t cap, const char *value)
{
    if (cap == 0) {
        return;
    }

    if (value == nullptr) {
        dst[0] = '\0';
        return;
    }

    strncpy(dst, value, cap - 1);
    dst[cap - 1] = '\0';
}

static char LowerAscii(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

static bool StartsWithWordNoCase(const char * text, const char * word)
{
    if (text == nullptr || word == nullptr) {
        return false;
    }

    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        ++text;
    }

    const char * p = text;
    const char * w = word;
    while (*w != '\0') {
        if (LowerAscii(*p) != LowerAscii(*w)) {
            return false;
        }
        ++p;
        ++w;
    }

    return *p == '\0' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n';
}

static bool ContainsWordNoCase(const char * text, const char * word)
{
    if (text == nullptr || word == nullptr || word[0] == '\0') {
        return false;
    }

    for (const char * p = text; *p != '\0'; ++p) {
        const bool left_ok = (p == text) || !((* (p - 1) >= 'A' && *(p - 1) <= 'Z') ||
                                             (*(p - 1) >= 'a' && *(p - 1) <= 'z') ||
                                             (*(p - 1) >= '0' && *(p - 1) <= '9') ||
                                             *(p - 1) == '_');
        if (!left_ok) {
            continue;
        }

        const char * a = p;
        const char * b = word;
        while (*b != '\0' && LowerAscii(*a) == LowerAscii(*b)) {
            ++a;
            ++b;
        }
        if (*b == '\0') {
            const bool right_ok = !((*a >= 'A' && *a <= 'Z') || (*a >= 'a' && *a <= 'z') ||
                                    (*a >= '0' && *a <= '9') || *a == '_');
            if (right_ok) {
                return true;
            }
        }
    }

    return false;
}

static bool IsReadOnlySql(const char * query)
{
    if (!StartsWithWordNoCase(query, "select")) {
        return false;
    }

    static const char * blocked[] = {
        "insert", "update", "delete", "merge", "drop", "alter", "truncate",
        "exec", "execute", "create", "grant", "revoke",
    };

    for (tdsl::uint32_t i = 0; i < sizeof(blocked) / sizeof(blocked[0]); ++i) {
        if (ContainsWordNoCase(query, blocked[i])) {
            return false;
        }
    }

    return true;
}

static bool IsWriteSqlAllowed(const char * query)
{
    if (!(StartsWithWordNoCase(query, "insert") || StartsWithWordNoCase(query, "update"))) {
        return false;
    }

    static const char * blocked[] = {
        "drop", "alter", "truncate", "grant", "revoke", "create", "merge", "delete",
        "exec", "execute",
    };

    for (tdsl::uint32_t i = 0; i < sizeof(blocked) / sizeof(blocked[0]); ++i) {
        if (ContainsWordNoCase(query, blocked[i])) {
            return false;
        }
    }

    return true;
}

static bool IsSafeSqlIdentifierForWeb(const char * text)
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }

    for (const char * p = text; *p != '\0'; ++p) {
        const char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            continue;
        }
        return false;
    }

    return true;
}

static bool WriteSqlLiteralForWeb(char * dst, tdsl::size_t cap, const char * value)
{
    if (dst == nullptr || value == nullptr || cap < 3) {
        return false;
    }

    tdsl::size_t out = 0;
    dst[out++] = '\'';
    for (const char * p = value; *p != '\0'; ++p) {
        if (out + 3 >= cap) {
            return false;
        }
        if (*p == '\'') {
            dst[out++] = '\'';
        }
        dst[out++] = *p;
    }
    dst[out++] = '\'';
    dst[out] = '\0';
    return true;
}

static bool AppendSql(char * dst, tdsl::size_t cap, const char * text)
{
    if (dst == nullptr || text == nullptr) {
        return false;
    }
    const tdsl::size_t used = strlen(dst);
    const tdsl::size_t add  = strlen(text);
    if (used + add + 1 >= cap) {
        return false;
    }
    strcat(dst, text);
    return true;
}

static bool AppendSqlIdentifier(char * dst, tdsl::size_t cap, const char * identifier)
{
    if (!IsSafeSqlIdentifierForWeb(identifier)) {
        return false;
    }

    return AppendSql(dst, cap, "[") && AppendSql(dst, cap, identifier) &&
           AppendSql(dst, cap, "]");
}

static void WebFormField(int sock, const char *value)
{
    HtmlWriteEscaped(sock, value != nullptr ? value : "");
}

void WebFormServer(int sock, PCSTR /*url*/)
{
    WebFormField(sock, SqlRuntimeGetConfig().server);
}

void WebFormPort(int sock, PCSTR /*url*/)
{
    tdsl::uint16_t port = SqlRuntimeGetConfig().port;
    if (port == 0) {
        port = SQL_DEFAULT_PORT;
    }
    fdprintf(sock, "%u", static_cast<unsigned>(port));
}

void WebFormUser(int sock, PCSTR /*url*/)
{
    WebFormField(sock, SqlRuntimeGetConfig().user);
}

void WebFormDatabase(int sock, PCSTR /*url*/)
{
    WebFormField(sock, SqlRuntimeGetConfig().database);
}

static void WriteCatalogSelectOptions(int sock, const char * selected,
                                      const char names[][SQL_CATALOG_NAME_LEN],
                                      tdsl::uint32_t count, bool include_manual)
{
    if (include_manual) {
        fdprintf(sock, "<option value=\"__manual__\">Type manually...</option>");
    }
    else {
        fdprintf(sock, "<option value=\"\">Select...</option>");
    }

    for (tdsl::uint32_t i = 0; i < count; ++i) {
        const char * name = names[i];
        fdprintf(sock, "<option value=\"");
        HtmlWriteEscaped(sock, name);
        fdprintf(sock, "\"");
        if (selected[0] != '\0' && strcmp(selected, name) == 0) {
            fdprintf(sock, " selected");
        }
        fdprintf(sock, ">");
        HtmlWriteEscaped(sock, name);
        fdprintf(sock, "</option>");
    }
}

void ShowDatabaseControl(int sock, PCSTR /*url*/)
{
    const sql_database_catalog & catalog = SqlDatabaseCatalogGet();
    const sql_runtime_config & cfg         = SqlRuntimeGetConfig();
    const char * selected                  = cfg.database;
    const bool has_list =
        catalog.ready && catalog.success && catalog.database_count > 0;

    if (has_list) {
        fdprintf(sock,
                 "<select name=\"database\" id=\"database-field\" class=\"nb-select-full\" "
                 "required>");
        WriteCatalogSelectOptions(sock, selected, catalog.database_names,
                                  catalog.database_count, true);
        fdprintf(sock, "</select>");
        fdprintf(sock,
                 "<label class=\"nb-field nb-field-manual\" id=\"database-manual-wrap\" hidden>"
                 "<span>Database name</span>"
                 "<input type=\"text\" name=\"database_manual\" id=\"database-manual\" "
                 "autocomplete=\"off\"></label>");
        return;
    }

    fdprintf(sock,
             "<input type=\"text\" name=\"database\" id=\"database-field\" "
             "class=\"nb-combo-input\" value=\"");
    HtmlWriteEscaped(sock, selected);
    fdprintf(sock, "\" placeholder=\"Database name\" autocomplete=\"off\" required>");
}

void ShowDatabaseBrowseStatus(int sock, PCSTR /*url*/)
{
    const sql_database_catalog & catalog = SqlDatabaseCatalogGet();

    if (SqlRuntimeIsBrowsingDatabases()) {
        fdprintf(sock, "Loading database list...");
        return;
    }

    if (!catalog.ready) {
        fdprintf(sock, "Click <strong>Refresh Databases</strong> to load databases from the server.");
        return;
    }

    if (!catalog.success) {
        fdprintf(sock, "<span class=\"nb-status nb-status-err\">");
        HtmlWriteEscaped(sock, catalog.status);
        fdprintf(sock, "</span>");
        return;
    }

    fdprintf(sock, "%lu database(s) loaded — pick from the list or type a name.",
             static_cast<unsigned long>(catalog.database_count));
}

void ShowTableControl(int sock, PCSTR /*url*/)
{
    const sql_table_catalog & catalog = SqlCatalogGet();
    const sql_runtime_config & cfg    = SqlRuntimeGetConfig();
    const char * selected             = cfg.table;
    const char * database             = cfg.database;
    const bool has_list = catalog.ready && catalog.success && catalog.table_count > 0 &&
                          database[0] != '\0' && strcmp(catalog.database, database) == 0;

    if (has_list) {
        fdprintf(sock,
                 "<select name=\"table\" id=\"table-field\" class=\"nb-select-full\">");
        WriteCatalogSelectOptions(sock, selected, catalog.table_names, catalog.table_count, true);
        fdprintf(sock, "</select>");
        fdprintf(sock,
                 "<label class=\"nb-field nb-field-manual\" id=\"table-manual-wrap\" hidden>"
                 "<span>Table name</span>"
                 "<input type=\"text\" name=\"table_manual\" id=\"table-manual\" "
                 "autocomplete=\"off\"></label>");
        return;
    }

    fdprintf(sock,
             "<input type=\"text\" name=\"table\" id=\"table-field\" class=\"nb-combo-input\" "
             "value=\"");
    HtmlWriteEscaped(sock, selected);
    fdprintf(sock, "\" placeholder=\"Table or view name\" autocomplete=\"off\">");
}

void ShowWriteTableControl(int sock, PCSTR /*url*/)
{
    const sql_table_catalog & catalog = SqlCatalogGet();
    const sql_runtime_config & cfg    = SqlRuntimeGetConfig();
    const char * selected             = cfg.table;
    const char * database             = cfg.database;
    const bool has_list = catalog.ready && catalog.success && catalog.table_count > 0 &&
                          database[0] != '\0' && strcmp(catalog.database, database) == 0;

    fdprintf(sock,
             "<select name=\"table\" id=\"table-field\" class=\"nb-select-full\" required>");

    if (has_list) {
        WriteCatalogSelectOptions(sock, selected, catalog.table_names, catalog.table_count, false);
    }
    else {
        fdprintf(sock, "<option value=\"\">Refresh tables to load list</option>");
    }

    fdprintf(sock, "</select>");
}

void ShowWriteTableBrowseStatus(int sock, PCSTR /*url*/)
{
    const sql_table_catalog & catalog = SqlCatalogGet();
    const char * database             = SqlRuntimeGetConfig().database;

    if (SqlRuntimeIsBrowsingTables()) {
        fdprintf(sock, "Loading table list...");
        return;
    }

    if (!catalog.ready) {
        fdprintf(sock, "Click <strong>Refresh Tables</strong> to load tables from the server.");
        return;
    }

    if (catalog.success && database[0] != '\0' && strcmp(catalog.database, database) != 0) {
        fdprintf(sock,
                 "Table list is for <span class=\"nb-mono\">");
        HtmlWriteEscaped(sock, catalog.database);
        fdprintf(sock, "</span>. Click Refresh Tables to refresh.");
        return;
    }

    if (!catalog.success) {
        fdprintf(sock, "<span class=\"nb-status nb-status-err\">");
        HtmlWriteEscaped(sock, catalog.status);
        fdprintf(sock, "</span>");
        return;
    }

    fdprintf(sock, "%lu table(s) loaded — pick a table from the list.",
             static_cast<unsigned long>(catalog.table_count));
}

void WebFormTable(int sock, PCSTR /*url*/)
{
    WebFormField(sock, SqlRuntimeGetConfig().table);
}

void WebFormWritePreview(int sock, PCSTR /*url*/)
{
    const sql_pending_mutation & pending = SqlRuntimeGetPendingMutation();
    if (pending.valid) {
        WebFormField(sock, pending.sql);
    }
}

void ShowTableBrowseStatus(int sock, PCSTR /*url*/)
{
    const sql_table_catalog & catalog = SqlCatalogGet();
    const char * database             = SqlRuntimeGetConfig().database;

    if (SqlRuntimeIsBrowsingTables()) {
        fdprintf(sock, "Loading table list...");
        return;
    }

    if (!catalog.ready) {
        fdprintf(sock, "Test connection, then click <strong>Refresh Tables</strong> to load tables.");
        return;
    }

    if (catalog.success && database[0] != '\0' && strcmp(catalog.database, database) != 0) {
        fdprintf(sock,
                 "Table list is for <span class=\"nb-mono\">");
        HtmlWriteEscaped(sock, catalog.database);
        fdprintf(sock, "</span>. Click Refresh Tables to refresh.");
        return;
    }

    if (!catalog.success) {
        fdprintf(sock, "<span class=\"nb-status nb-status-err\">");
        HtmlWriteEscaped(sock, catalog.status);
        fdprintf(sock, "</span>");
        return;
    }

    fdprintf(sock, "%lu table(s) loaded — pick from the list or type a name.",
             static_cast<unsigned long>(catalog.table_count));
}

static bool ColumnSelected(const char * columns, const char * name)
{
    if (columns == nullptr || name == nullptr || columns[0] == '\0') {
        return false;
    }

    if (strcmp(columns, "*") == 0) {
        return true;
    }

    char bracketed[SQL_CATALOG_NAME_LEN + 3] = {};
    snprintf(bracketed, sizeof(bracketed), "[%s]", name);
    return strstr(columns, bracketed) != nullptr;
}

void ShowColumnControl(int sock, PCSTR /*url*/)
{
    const sql_column_catalog & catalog = SqlColumnCatalogGet();
    const sql_runtime_config & cfg     = SqlRuntimeGetConfig();
    const bool has_list = catalog.ready && catalog.success && catalog.column_count > 0 &&
                          cfg.database[0] != '\0' && cfg.table[0] != '\0' &&
                          strcmp(catalog.database, cfg.database) == 0 &&
                          strcmp(catalog.table, cfg.table) == 0;

    if (!has_list) {
        fdprintf(sock,
                 "<label class=\"nb-field\"><span>Columns</span>"
                 "<input type=\"text\" name=\"columns\" id=\"columns-field\" value=\"");
        HtmlWriteEscaped(sock, cfg.columns);
        fdprintf(sock, "\" placeholder=\"* or [Id],[Name]\"></label>");
        return;
    }

    fdprintf(sock, "<input type=\"hidden\" name=\"columns\" value=\"\">");
    fdprintf(sock, "<div class=\"nb-column-picker\" id=\"columns-field\">");
    fdprintf(sock, "<div class=\"nb-column-picker-head\"><span>Columns</span>"
                   "<span class=\"nb-field-hint\">Select columns in table order</span></div>");

    for (tdsl::uint32_t i = 0; i < catalog.column_count; ++i) {
        const char * name = catalog.column_names[i];
        fdprintf(sock, "<label class=\"nb-column-option\"><input type=\"checkbox\" name=\"columns\" value=\"[");
        HtmlWriteEscaped(sock, name);
        fdprintf(sock, "]\"");
        if (ColumnSelected(cfg.columns, name)) {
            fdprintf(sock, " checked");
        }
        fdprintf(sock, "><span>");
        HtmlWriteEscaped(sock, name);
        fdprintf(sock, "</span><em>");
        HtmlWriteEscaped(sock, catalog.column_types[i]);
        fdprintf(sock, "</em></label>");
    }

    fdprintf(sock, "</div>");
}

void ShowColumnBrowseStatus(int sock, PCSTR /*url*/)
{
    const sql_column_catalog & catalog = SqlColumnCatalogGet();
    const sql_runtime_config & cfg     = SqlRuntimeGetConfig();

    if (SqlRuntimeIsBrowsingColumns()) {
        fdprintf(sock, "Loading column list...");
        return;
    }

    if (!catalog.ready) {
        fdprintf(sock, "Pick a table, then click <strong>Refresh Columns</strong>.");
        return;
    }

    if (catalog.success && cfg.database[0] != '\0' && cfg.table[0] != '\0' &&
        (strcmp(catalog.database, cfg.database) != 0 || strcmp(catalog.table, cfg.table) != 0)) {
        fdprintf(sock, "Column list is for <span class=\"nb-mono\">");
        HtmlWriteEscaped(sock, catalog.table);
        fdprintf(sock, "</span>. Click Refresh Columns to refresh.");
        return;
    }

    if (!catalog.success) {
        fdprintf(sock, "<span class=\"nb-status nb-status-err\">");
        HtmlWriteEscaped(sock, catalog.status);
        fdprintf(sock, "</span>");
        return;
    }

    fdprintf(sock, "%lu column(s) loaded.", static_cast<unsigned long>(catalog.column_count));
}

void ShowWriteColumnStatus(int sock, PCSTR /*url*/)
{
    const sql_column_catalog & catalog = SqlColumnCatalogGet();
    const sql_runtime_config & cfg     = SqlRuntimeGetConfig();

    if (SqlRuntimeIsBrowsingColumns()) {
        fdprintf(sock, "Loading column list...");
        return;
    }

    if (cfg.table[0] == '\0') {
        fdprintf(sock, "Pick a table, then click <strong>Load Columns</strong>.");
        return;
    }

    if (!catalog.ready) {
        fdprintf(sock, "Click <strong>Load Columns</strong> to fetch fields for the selected table.");
        return;
    }

    if (catalog.success && cfg.database[0] != '\0' && cfg.table[0] != '\0' &&
        (strcmp(catalog.database, cfg.database) != 0 || strcmp(catalog.table, cfg.table) != 0)) {
        fdprintf(sock, "Column list is for <span class=\"nb-mono\">");
        HtmlWriteEscaped(sock, catalog.table);
        fdprintf(sock, "</span>. Click Load Columns to refresh.");
        return;
    }

    if (!catalog.success) {
        fdprintf(sock, "<span class=\"nb-status nb-status-err\">");
        HtmlWriteEscaped(sock, catalog.status);
        fdprintf(sock, "</span>");
        return;
    }

    fdprintf(sock, "%lu column(s) loaded for guided insert and update.",
             static_cast<unsigned long>(catalog.column_count));
}

static void ShowBuilderColumnSelect(int sock, const char * name, const char * id,
                                    const char * selected, const char * placeholder)
{
    const sql_column_catalog & catalog = SqlColumnCatalogGet();
    const sql_runtime_config & cfg     = SqlRuntimeGetConfig();
    const bool has_list = catalog.ready && catalog.success && catalog.column_count > 0 &&
                          cfg.database[0] != '\0' && cfg.table[0] != '\0' &&
                          strcmp(catalog.database, cfg.database) == 0 &&
                          strcmp(catalog.table, cfg.table) == 0;

    fdprintf(sock, "<select name=\"%s\" id=\"%s\">", name, id);
    fdprintf(sock, "<option value=\"\">%s</option>", placeholder);
    if (has_list) {
        for (tdsl::uint32_t i = 0; i < catalog.column_count; ++i) {
            const char * column = catalog.column_names[i];
            fdprintf(sock, "<option value=\"");
            HtmlWriteEscaped(sock, column);
            fdprintf(sock, "\"");
            if (selected != nullptr && strcmp(selected, column) == 0) {
                fdprintf(sock, " selected");
            }
            fdprintf(sock, ">");
            HtmlWriteEscaped(sock, column);
            fdprintf(sock, "</option>");
        }
    }
    fdprintf(sock, "</select>");
}

void ShowFilterColumnControl(int sock, PCSTR /*url*/)
{
    ShowBuilderColumnSelect(sock, "filter_column", "filter-column-field",
                            SqlRuntimeGetConfig().filter_column, "No filter");
}

void ShowSortColumnControl(int sock, PCSTR /*url*/)
{
    ShowBuilderColumnSelect(sock, "sort_column", "sort-column-field",
                            SqlRuntimeGetConfig().sort_column, "No sort");
}

void ShowFilterOpOptions(int sock, PCSTR /*url*/)
{
    const char * selected = SqlRuntimeGetConfig().filter_op;
    struct option_pair {
        const char * value;
        const char * label;
    };
    static const option_pair options[] = {
        {"eq", "Equals"},       {"ne", "Not equal"},      {"gt", "Greater than"},
        {"ge", "Greater/equal"}, {"lt", "Less than"},      {"le", "Less/equal"},
        {"contains", "Contains"}, {"starts", "Starts with"}, {"ends", "Ends with"},
        {"isnull", "Is null"},  {"isnotnull", "Is not null"},
    };

    for (tdsl::uint32_t i = 0; i < sizeof(options) / sizeof(options[0]); ++i) {
        fdprintf(sock, "<option value=\"%s\"", options[i].value);
        if (selected[0] != '\0' && strcmp(selected, options[i].value) == 0) {
            fdprintf(sock, " selected");
        }
        fdprintf(sock, ">%s</option>", options[i].label);
    }
}

void WebFormFilterValue(int sock, PCSTR /*url*/)
{
    WebFormField(sock, SqlRuntimeGetConfig().filter_value);
}

void ShowSortDescSelected(int sock, PCSTR /*url*/)
{
    if (SqlRuntimeGetConfig().sort_desc) {
        fdprintf(sock, " selected");
    }
}

static void WriteColumnEmptyState(int sock)
{
    const sql_runtime_config & cfg     = SqlRuntimeGetConfig();
    const sql_column_catalog & catalog = SqlColumnCatalogGet();

    if (cfg.database[0] == '\0' || cfg.table[0] == '\0') {
        if (cfg.database[0] == '\0') {
            fdprintf(sock,
                     "<p class=\"nb-empty-state\">Set a database on Configure, then pick a table above.</p>");
        }
        else {
            fdprintf(sock,
                     "<p class=\"nb-empty-state\">Pick a table above and click Load Columns.</p>");
        }
        return;
    }

    if (SqlRuntimeIsBrowsingColumns()) {
        fdprintf(sock, "<p class=\"nb-empty-state\">Loading columns...</p>");
        return;
    }

    if (!catalog.ready || !SqlColumnCatalogMatches(cfg.database, cfg.table)) {
        fdprintf(sock, "<p class=\"nb-empty-state\">Loading columns...</p>");
        return;
    }

    if (!catalog.success) {
        fdprintf(sock, "<p class=\"nb-empty-state\"><span class=\"nb-status nb-status-err\">");
        HtmlWriteEscaped(sock, catalog.status);
        fdprintf(sock, "</span></p>");
        return;
    }

    fdprintf(sock, "<p class=\"nb-empty-state\">No columns found for this table.</p>");
}

void ShowWriteColumnInputs(int sock, PCSTR /*url*/)
{
    const sql_column_catalog & catalog = SqlColumnCatalogGet();
    const sql_runtime_config & cfg     = SqlRuntimeGetConfig();
    const bool has_list = catalog.ready && catalog.success && catalog.column_count > 0 &&
                          cfg.database[0] != '\0' && cfg.table[0] != '\0' &&
                          strcmp(catalog.database, cfg.database) == 0 &&
                          strcmp(catalog.table, cfg.table) == 0;

    if (!has_list) {
        WriteColumnEmptyState(sock);
        return;
    }

    fdprintf(sock, "<div class=\"nb-write-values\">");
    for (tdsl::uint32_t i = 0; i < catalog.column_count; ++i) {
        fdprintf(sock, "<label class=\"nb-field nb-write-field\"><span>");
        HtmlWriteEscaped(sock, catalog.column_names[i]);
        fdprintf(sock, "</span>");
        fdprintf(sock, "<input type=\"hidden\" name=\"col%lu\" value=\"",
                 static_cast<unsigned long>(i));
        HtmlWriteEscaped(sock, catalog.column_names[i]);
        fdprintf(sock, "\">");
        fdprintf(sock, "<input type=\"text\" name=\"val%lu\" placeholder=\"",
                 static_cast<unsigned long>(i));
        HtmlWriteEscaped(sock, catalog.column_types[i]);
        fdprintf(sock, "\">");
        fdprintf(sock, "</label>");
    }
    fdprintf(sock, "</div>");
}

void ShowUpdateColumnInputs(int sock, PCSTR /*url*/)
{
    const sql_column_catalog & catalog = SqlColumnCatalogGet();
    const sql_runtime_config & cfg     = SqlRuntimeGetConfig();
    const bool has_list = catalog.ready && catalog.success && catalog.column_count > 0 &&
                          cfg.database[0] != '\0' && cfg.table[0] != '\0' &&
                          strcmp(catalog.database, cfg.database) == 0 &&
                          strcmp(catalog.table, cfg.table) == 0;

    if (!has_list) {
        WriteColumnEmptyState(sock);
        return;
    }

    fdprintf(sock, "<div class=\"nb-write-values\">");
    for (tdsl::uint32_t i = 0; i < catalog.column_count; ++i) {
        fdprintf(sock, "<label class=\"nb-field nb-write-field\"><span>");
        HtmlWriteEscaped(sock, catalog.column_names[i]);
        fdprintf(sock, "</span>");
        fdprintf(sock, "<input type=\"hidden\" name=\"col%lu\" value=\"",
                 static_cast<unsigned long>(i));
        HtmlWriteEscaped(sock, catalog.column_names[i]);
        fdprintf(sock, "\">");
        fdprintf(sock, "<input type=\"text\" name=\"val%lu\" placeholder=\"",
                 static_cast<unsigned long>(i));
        HtmlWriteEscaped(sock, catalog.column_types[i]);
        fdprintf(sock, "\">");
        fdprintf(sock, "</label>");
    }
    fdprintf(sock, "</div>");
}

void ShowWriteWhereColumnOptions(int sock, PCSTR /*url*/)
{
    const sql_column_catalog & catalog = SqlColumnCatalogGet();
    const sql_runtime_config & cfg     = SqlRuntimeGetConfig();
    const bool has_list = catalog.ready && catalog.success && catalog.column_count > 0 &&
                          cfg.database[0] != '\0' && cfg.table[0] != '\0' &&
                          strcmp(catalog.database, cfg.database) == 0 &&
                          strcmp(catalog.table, cfg.table) == 0;

    if (!has_list) {
        fdprintf(sock, "<option value=\"\">Refresh columns first</option>");
        return;
    }

    fdprintf(sock, "<option value=\"\">Choose key column</option>");
    for (tdsl::uint32_t i = 0; i < catalog.column_count; ++i) {
        fdprintf(sock, "<option value=\"");
        HtmlWriteEscaped(sock, catalog.column_names[i]);
        fdprintf(sock, "\">");
        HtmlWriteEscaped(sock, catalog.column_names[i]);
        fdprintf(sock, "</option>");
    }
}

void ShowPendingMutation(int sock, PCSTR /*url*/)
{
    const sql_pending_mutation & pending = SqlRuntimeGetPendingMutation();

    if (!pending.valid) {
        fdprintf(sock,
                 "<p class=\"nb-empty-state\">No pending write operation. Build an insert or update first.</p>");
        return;
    }

    fdprintf(sock, "<p>");
    HtmlWriteEscaped(sock, pending.summary);
    fdprintf(sock, "</p><code class=\"nb-mono nb-confirm-sql\">");
    HtmlWriteEscaped(sock, pending.sql);
    fdprintf(sock, "</code>");
}

void ShowPendingConfirmDisabled(int sock, PCSTR /*url*/)
{
    if (!SqlRuntimeGetPendingMutation().valid) {
        fdprintf(sock, " disabled");
    }
}

void ShowPendingPreviewHidden(int sock, PCSTR /*url*/)
{
    if (!SqlRuntimeGetPendingMutation().valid) {
        fdprintf(sock, " hidden");
    }
}

static void WriteLastVerifiedLine(int sock)
{
    const tdsl::uint32_t verified_at = SqlRuntimeLastVerifiedSecs();
    if (verified_at == 0) {
        fdprintf(sock, "<p class=\"nb-conn-meta\">Last verified: not yet tested this session</p>");
        return;
    }

    if (Secs >= verified_at) {
        const unsigned long ago = Secs - verified_at;
        if (ago < 120) {
            fdprintf(sock, "<p class=\"nb-conn-meta\">Last verified: %lu second(s) ago</p>", ago);
            return;
        }
        fdprintf(sock, "<p class=\"nb-conn-meta\">Last verified: %lu minute(s) ago</p>",
                 ago / 60u);
        return;
    }

    fdprintf(sock, "<p class=\"nb-conn-meta\">Last verified: saved in NV flash</p>");
}

void ShowConnectionSummaryHidden(int sock, PCSTR /*url*/)
{
    if (!SqlRuntimeIsConnectionVerified()) {
        fdprintf(sock, " hidden");
    }
}

void ShowConnectionFormHidden(int sock, PCSTR /*url*/)
{
    if (SqlRuntimeIsConnectionVerified()) {
        fdprintf(sock, " hidden");
    }
}

void ShowConnectionSummaryActionsHidden(int sock, PCSTR /*url*/)
{
    if (!SqlRuntimeIsConnectionVerified()) {
        fdprintf(sock, " hidden");
    }
}

void ShowConnectionSummary(int sock, PCSTR /*url*/)
{
    if (!SqlRuntimeIsConnectionVerified()) {
        return;
    }

    const sql_runtime_config & cfg = SqlRuntimeGetConfig();
    tdsl::uint16_t port            = cfg.port;
    if (port == 0) {
        port = SQL_DEFAULT_PORT;
    }

    fdprintf(sock, "<div class=\"nb-conn-summary\">");
    fdprintf(sock, "<div class=\"nb-conn-badge nb-status nb-status-ok\">Connected</div>");
    fdprintf(sock, "<p class=\"nb-conn-host\">");
    HtmlWriteEscaped(sock, cfg.server);
    fdprintf(sock, ":%u</p>", static_cast<unsigned>(port));
    fdprintf(sock, "<p class=\"nb-conn-line\"><span>Database</span> ");
    HtmlWriteEscaped(sock, cfg.database);
    fdprintf(sock, "</p><p class=\"nb-conn-line\"><span>User</span> ");
    HtmlWriteEscaped(sock, cfg.user);
    fdprintf(sock, "</p>");
    WriteLastVerifiedLine(sock);
    fdprintf(sock, "</div>");
}

void ShowQuerySectionClass(int sock, PCSTR /*url*/)
{
    if (!SqlRuntimeIsConnectionVerified()) {
        fdprintf(sock, " nb-section-locked");
    }
}

void ShowQueryLockBanner(int sock, PCSTR /*url*/)
{
    if (SqlRuntimeIsConnectionVerified()) {
        return;
    }

    fdprintf(sock,
             "<p class=\"nb-section-lock\">Complete <strong>Test Connection</strong> in step 1 "
             "to browse tables and run queries.</p>");
}

void ShowConnectedDatabase(int sock, PCSTR /*url*/)
{
    HtmlWriteEscaped(sock, SqlRuntimeGetConfig().database);
}

void ShowHeaderStatus(int sock, PCSTR /*url*/)
{
    if (SqlRuntimeIsTestingConnection()) {
        fdprintf(sock, "<span class=\"nb-header-status nb-header-status-busy\">Testing connection</span>");
        return;
    }

    if (SqlRuntimeIsBrowsingDatabases() || SqlRuntimeIsBrowsingTables() ||
        SqlRuntimeIsBrowsingColumns()) {
        fdprintf(sock, "<span class=\"nb-header-status nb-header-status-busy\">Loading catalog</span>");
        return;
    }

    if (SqlRuntimeIsRunningQuery()) {
        fdprintf(sock, "<span class=\"nb-header-status nb-header-status-busy\">Query running</span>");
        return;
    }

    if (SqlRuntimeIsExecutingMutation()) {
        fdprintf(sock, "<span class=\"nb-header-status nb-header-status-busy\">Gateway writing</span>");
        return;
    }

    const sql_result_store & store = SqlResultsGet();
    if (store.ready && !store.success) {
        fdprintf(sock, "<span class=\"nb-header-status nb-header-status-error\">Query failed</span>");
        return;
    }

    const sql_connection_status & status = SqlConnectionStatusGet();
    if (status.ready && !status.success) {
        fdprintf(sock, "<span class=\"nb-header-status nb-header-status-error\">Connection failed</span>");
        return;
    }

    if (SqlRuntimeIsConnectionVerified()) {
        fdprintf(sock, "<span class=\"nb-header-status nb-header-status-ok\">SQL connected</span>");
        return;
    }

    fdprintf(sock, "<span class=\"nb-header-status nb-header-status-idle\">Disconnected</span>");
}

void ShowHeaderTarget(int sock, PCSTR /*url*/)
{
    const sql_runtime_config & cfg = SqlRuntimeGetConfig();
    if (cfg.server[0] == '\0') {
        fdprintf(sock, "No SQL target");
        return;
    }

    HtmlWriteEscaped(sock, cfg.server);
    if (cfg.port != 0) {
        fdprintf(sock, ":%u", static_cast<unsigned>(cfg.port));
    }
}

#ifndef NB_GATEWAY_MICRO800
static void WriteNavLink(int sock, PCSTR url, const char *href, const char *label)
{
    const bool active = (url && strstr(url, href) != nullptr);
    fdprintf(sock, "<a class=\"nb-nav-link%s\" href=\"%s\">%s</a>", active ? " nb-nav-link-active" : "", href,
             label);
}

void ShowAppNav(int sock, PCSTR url)
{
    const bool onOther =
        url && (strstr(url, "configure") || strstr(url, "browse") || strstr(url, "write") ||
                strstr(url, "results") || strstr(url, "diagnostic") || strstr(url, "confirm"));
    const bool dash = !onOther;
    fdprintf(sock, "<a class=\"nb-nav-link%s\" href=\"index.html\">Dashboard</a>",
             dash ? " nb-nav-link-active" : "");
    WriteNavLink(sock, url, "configure.html", "Configure");
    WriteNavLink(sock, url, "browse.html", "Browse");
    WriteNavLink(sock, url, "write.html", "Insert / Update");
    WriteNavLink(sock, url, "results.html", "Results");
    WriteNavLink(sock, url, "diagnostics.html", "Diagnostics");
}

void ShowGatewayNavLinks(int /*sock*/, PCSTR /*url*/) {}
void ShowGatewayOverview(int sock, PCSTR /*url*/)
{
    writestring(sock, "<p>Gateway build not enabled.</p>");
}
#endif

static bool IsUnsupportedCell(const char * text)
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }

    return text[0] == '?' ||
           (text[0] == '0' && text[1] == 'x' && text[2] != '\0');
}

static void HtmlWriteResultCell(int sock, const char * text)
{
    if (text == nullptr || text[0] == '\0') {
        fdprintf(sock, "<span class=\"nb-cell nb-cell-empty\" title=\"Empty string\">(empty)</span>");
        return;
    }

    if (strcmp(text, "NULL") == 0) {
        fdprintf(sock, "<span class=\"nb-cell nb-cell-null\" title=\"SQL NULL\">NULL</span>");
        return;
    }

    if (IsUnsupportedCell(text)) {
        fdprintf(sock, "<span class=\"nb-cell nb-cell-unsupported\" title=\"Unsupported TDS value: ");
        HtmlWriteEscaped(sock, text);
        fdprintf(sock, "\">Unsupported type</span>");
        return;
    }

    HtmlWriteEscaped(sock, text);
}

void ShowResultsBanner(int sock, PCSTR /*url*/)
{
    const sql_result_store & store = SqlResultsGet();

    if (SqlRuntimeIsRunningQuery()) {
        fdprintf(sock, "<p class=\"nb-results-banner nb-results-running\">Running query...</p>");
        return;
    }

    if (SqlRuntimeIsExecutingMutation()) {
        fdprintf(sock, "<p class=\"nb-results-banner nb-results-running\">Writing to SQL...</p>");
        return;
    }

    if (!store.ready) {
        fdprintf(sock,
                 "<p class=\"nb-results-banner\">Run a query in step 2 to preview data here.</p>");
        return;
    }

    if (!store.success) {
        fdprintf(sock, "<p class=\"nb-results-banner nb-results-error\">");
        if (strstr(store.status, "login") != nullptr || strstr(store.status, "Connection") != nullptr ||
            strstr(store.status, "connect") != nullptr) {
            fdprintf(sock, "<span class=\"nb-status nb-status-err\">Connection error:</span> ");
        }
        else {
            fdprintf(sock, "<span class=\"nb-status nb-status-err\">Query error:</span> ");
        }
        HtmlWriteEscaped(sock, store.status);
        fdprintf(sock, "</p>");
        return;
    }

    fdprintf(sock, "<p class=\"nb-results-banner nb-results-ok\">");
    if (store.is_mutation) {
        HtmlWriteEscaped(sock, store.status[0] != '\0' ? store.status : "Write completed.");
        fdprintf(sock, " in %lu ms", static_cast<unsigned long>(store.query_duration_ms));
    }
    else {
        fdprintf(sock, "%lu row(s) returned in %lu ms",
                 static_cast<unsigned long>(store.row_count),
                 static_cast<unsigned long>(store.query_duration_ms));
    }
    if (store.table_label[0] != '\0') {
        fdprintf(sock, " from <span class=\"nb-mono\">");
        HtmlWriteEscaped(sock, store.table_label);
        fdprintf(sock, "</span>");
    }
    if (store.row_count >= SQL_RESULT_MAX_ROWS) {
        fdprintf(sock, " — display limit %u rows", SQL_RESULT_MAX_ROWS);
    }
    fdprintf(sock, "</p>");
}

void WebFormColumns(int sock, PCSTR /*url*/)
{
    WebFormField(sock, SqlRuntimeGetConfig().columns);
}

void WebFormTopN(int sock, PCSTR /*url*/)
{
    tdsl::uint32_t top_n = SqlRuntimeGetConfig().top_n;
    if (top_n == 0) {
        top_n = SQL_DEFAULT_TOP_N;
    }
    fdprintf(sock, "%lu", static_cast<unsigned long>(top_n));
}

void ShowTopNOptions(int sock, PCSTR /*url*/)
{
    tdsl::uint32_t top_n = SqlRuntimeGetConfig().top_n;
    if (top_n == 0 || top_n > SQL_MAX_TOP_N) {
        top_n = SQL_DEFAULT_TOP_N;
    }

    static const tdsl::uint32_t options[] = {10, 25, 50, 100, 250};
    for (tdsl::uint32_t i = 0; i < sizeof(options) / sizeof(options[0]); ++i) {
        fdprintf(sock, "<option value=\"%lu\"", static_cast<unsigned long>(options[i]));
        if (top_n == options[i]) {
            fdprintf(sock, " selected");
        }
        fdprintf(sock, ">%lu</option>", static_cast<unsigned long>(options[i]));
    }
}

void WebFormQuery(int sock, PCSTR /*url*/)
{
    WebFormField(sock, SqlRuntimeGetConfig().query);
}

void WebFormUseTableBuilderChecked(int sock, PCSTR /*url*/)
{
    const sql_runtime_config & cfg = SqlRuntimeGetConfig();
    if (cfg.use_table_builder || cfg.query[0] == '\0') {
        fdprintf(sock, "checked");
    }
}

void ShowFormScript(int sock, PCSTR /*url*/)
{
    fdprintf(sock,
             "<script>\n"
             "(function(){\n"
             "  function byId(id){return document.getElementById(id);}\n"
             "  function setHidden(el,hidden){if(el){el.hidden=hidden;}}\n"
             "  function showConnectionEditor(show){\n"
             "    var summary=byId('connection-summary');\n"
             "    var actions=byId('connection-summary-actions');\n"
             "    var form=byId('connection-form-panel');\n"
             "    setHidden(summary,show);\n"
             "    setHidden(actions,show);\n"
             "    setHidden(form,!show);\n"
             "  }\n"
             "  var changeBtn=byId('change-connection-btn');\n"
             "  if(changeBtn){changeBtn.addEventListener('click',function(){showConnectionEditor(true);});}\n"
             "  function bindManualSelect(selectId,wrapId,manualId){\n"
             "    var sel=byId(selectId);\n"
             "    var wrap=byId(wrapId);\n"
             "    var manual=manualId?byId(manualId):null;\n"
             "    if(!sel||!wrap){return;}\n"
             "    function sync(){\n"
             "      var manualMode=sel.value==='__manual__';\n"
             "      wrap.hidden=!manualMode;\n"
             "      if(manual){manual.required=manualMode;}\n"
             "      sel.required=!manualMode;\n"
             "    }\n"
             "    sel.addEventListener('change',sync);\n"
             "    sync();\n"
             "  }\n"
             "  bindManualSelect('database-field','database-manual-wrap','database-manual');\n"
             "  bindManualSelect('table-field','table-manual-wrap','table-manual');\n"
             "  var builderEl=byId('use-table-builder');\n"
             "  var queryEl=byId('query-field');\n"
             "  var builderPanel=byId('builder-panel');\n"
             "  var customPanel=byId('custom-panel');\n"
             "  var previewLabel=byId('sql-preview-label');\n"
             "  var previewHint=byId('sql-preview-hint');\n"
             "  var queryForm=byId('query-form');\n"
             "  var querySqlSection=byId('query-sql-section');\n"
             "  var modeButtons=document.querySelectorAll('[data-query-mode]');\n"
             "  var dbHidden=byId('connected-database');\n"
             "  function getDatabaseName(){\n"
             "    if(dbHidden&&dbHidden.value){return dbHidden.value;}\n"
             "    var dbField=byId('database-field');\n"
             "    if(!dbField){return '';}\n"
             "    if(dbField.tagName==='SELECT'){\n"
             "      if(dbField.value==='__manual__'){\n"
             "        var m=byId('database-manual');\n"
             "        return m?m.value:'';\n"
             "      }\n"
             "      return dbField.value;\n"
             "    }\n"
             "    return dbField.value;\n"
             "  }\n"
             "  function getTableName(){\n"
             "    var tableField=byId('table-field');\n"
             "    if(!tableField){return '';}\n"
             "    if(tableField.tagName==='SELECT'){\n"
             "      if(tableField.value==='__manual__'){\n"
             "        var m=byId('table-manual');\n"
             "        return m?m.value:'';\n"
             "      }\n"
             "      return tableField.value;\n"
             "    }\n"
             "    return tableField.value;\n"
             "  }\n"
             "  function getColumnText(){\n"
             "    var colsEl=byId('columns-field');\n"
             "    if(!colsEl){return '*';}\n"
             "    if(colsEl.tagName==='DIV'){\n"
             "      var selected=[];\n"
             "      colsEl.querySelectorAll('input[name=\"columns\"]:checked').forEach(function(cb){selected.push(cb.value);});\n"
             "      return selected.length?selected.join(','):'*';\n"
             "    }\n"
             "    return colsEl.value?colsEl.value:'*';\n"
             "  }\n"
             "  function sqlLiteral(value){return '\\''+String(value).replace(/'/g,\"''\")+'\\'';}\n"
             "  function appendFilter(sql){\n"
             "    var col=byId('filter-column-field');\n"
             "    var op=byId('filter-op-field');\n"
             "    var val=byId('filter-value-field');\n"
             "    if(!col||!op||!col.value||!op.value){return sql;}\n"
             "    if(op.value==='isnull'){return sql+' WHERE ['+col.value+'] IS NULL';}\n"
             "    if(op.value==='isnotnull'){return sql+' WHERE ['+col.value+'] IS NOT NULL';}\n"
             "    if(!val||!val.value){return sql;}\n"
             "    var sqlOp={'eq':'=','ne':'<>','gt':'>','ge':'>=','lt':'<','le':'<=','contains':'LIKE','starts':'LIKE','ends':'LIKE'}[op.value];\n"
             "    if(!sqlOp){return sql;}\n"
             "    var v=val.value;\n"
             "    if(op.value==='contains'){v='\\x25'+v+'\\x25';}\n"
             "    if(op.value==='starts'){v=v+'\\x25';}\n"
             "    if(op.value==='ends'){v='\\x25'+v;}\n"
             "    return sql+' WHERE ['+col.value+'] '+sqlOp+' '+sqlLiteral(v);\n"
             "  }\n"
             "  function appendSort(sql){\n"
             "    var col=byId('sort-column-field');\n"
             "    var dir=byId('sort-desc-field');\n"
             "    if(!col||!col.value){return sql;}\n"
             "    return sql+' ORDER BY ['+col.value+'] '+(dir&&dir.value==='desc'?'DESC':'ASC');\n"
             "  }\n"
             "  function setQueryMode(mode){\n"
             "    var builderMode=mode==='builder';\n"
             "    if(builderEl){builderEl.checked=builderMode;}\n"
             "    if(queryForm){\n"
             "      queryForm.classList.toggle('nb-browse-mode-builder',builderMode);\n"
             "      queryForm.classList.toggle('nb-browse-mode-custom',!builderMode);\n"
             "    }\n"
             "    setHidden(builderPanel,!builderMode);\n"
             "    setHidden(customPanel,builderMode);\n"
             "    if(querySqlSection){querySqlSection.hidden=false;}\n"
             "    if(previewLabel){previewLabel.textContent=builderMode?'Generated SQL':'Custom SQL';}\n"
             "    if(previewHint){previewHint.textContent=builderMode?'Read-only preview from Builder controls.':'Edit your SELECT statement below, then run query.';}\n"
             "    for(var i=0;i<modeButtons.length;++i){\n"
             "      var btn=modeButtons[i];\n"
             "      var active=btn.getAttribute('data-query-mode')===mode;\n"
             "      btn.classList.toggle('nb-mode-active',active);\n"
             "      btn.setAttribute('aria-pressed',active?'true':'false');\n"
             "    }\n"
             "    syncQueryFieldMode();\n"
             "    if(!builderMode&&queryEl){queryEl.focus();}\n"
             "  }\n"
             "  function syncQueryFieldMode(){\n"
             "    if(!builderEl||!queryEl){return;}\n"
             "    if(builderEl.checked){\n"
             "      queryEl.readOnly=true;\n"
             "      queryEl.classList.add('nb-readonly');\n"
             "      updateTableBuilderPreview();\n"
             "    }else{\n"
             "      queryEl.readOnly=false;\n"
             "      queryEl.classList.remove('nb-readonly');\n"
             "    }\n"
             "  }\n"
             "  function updateTableBuilderPreview(){\n"
             "    if(!builderEl||!queryEl||!builderEl.checked){return;}\n"
             "    var db=getDatabaseName();\n"
             "    var table=getTableName();\n"
             "    var topEl=byId('top-n-field');\n"
             "    var cols=getColumnText();\n"
             "    var topN=topEl&&topEl.value?topEl.value:'100';\n"
             "    if(!db||!table){queryEl.value='';return;}\n"
             "    var sql='SELECT TOP ('+topN+') '+cols+' FROM ['+db+'].[dbo].['+table+']';\n"
             "    queryEl.value=appendSort(appendFilter(sql));\n"
             "  }\n"
             "  for(var mb=0;mb<modeButtons.length;++mb){\n"
             "    modeButtons[mb].addEventListener('click',function(){\n"
             "      setQueryMode(this.getAttribute('data-query-mode'));\n"
             "    });\n"
             "  }\n"
             "  if(queryEl){queryEl.addEventListener('input',function(){if(builderEl&&!builderEl.checked){syncQueryFieldMode();}});}\n"
             "  var tableField=byId('table-field');\n"
             "  if(tableField){\n"
             "    tableField.addEventListener('change',function(){\n"
             "      if(builderEl&&this.value&&this.value!=='__manual__'){setQueryMode('builder');}\n"
             "      syncQueryFieldMode();\n"
             "    });\n"
             "  }\n"
             "  ['columns-field','top-n-field','filter-column-field','filter-op-field','filter-value-field','sort-column-field','sort-desc-field'].forEach(function(id){\n"
             "    var el=byId(id);\n"
             "    if(!el){return;}\n"
             "    el.addEventListener('input',updateTableBuilderPreview);\n"
             "    el.addEventListener('change',updateTableBuilderPreview);\n"
             "  });\n"
             "  if(builderPanel||customPanel||queryEl){\n"
             "    setQueryMode(builderEl&&builderEl.checked?'builder':'custom');\n"
             "  }\n"
             "  var writePreviewEl=byId('write-preview-field');\n"
             "  if(writePreviewEl){\n"
             "    var writePreviewLabel=byId('write-preview-label');\n"
             "    var writePreviewHint=byId('write-preview-hint');\n"
             "    var stagedWritePreview=writePreviewEl.value||'';\n"
             "    function isSafeWriteIdentifier(name){\n"
             "      if(!name){return false;}\n"
             "      return /^[A-Za-z0-9_]+$/.test(name);\n"
             "    }\n"
             "    function sqlWriteIdent(name){return isSafeWriteIdentifier(name)?'['+name+']':'';}\n"
             "    function getWriteDatabase(){\n"
             "      if(dbHidden&&dbHidden.value){return dbHidden.value;}\n"
             "      var dbField=byId('connected-database');\n"
             "      return dbField?dbField.value:'';\n"
             "    }\n"
             "    function getWriteTable(){\n"
             "      var tableField=byId('table-field');\n"
             "      if(tableField){return getTableName();}\n"
             "      var hidden=byId('connected-table');\n"
             "      return hidden?hidden.value:'';\n"
             "    }\n"
             "    function collectWritePairs(form){\n"
             "      var pairs=[];\n"
             "      if(!form){return pairs;}\n"
             "      form.querySelectorAll('input[type=\"hidden\"][name^=\"col\"]').forEach(function(colInput){\n"
             "        var match=colInput.name.match(/^col(\\d+)$/);\n"
             "        if(!match){return;}\n"
             "        var valInput=form.querySelector('input[name=\"val'+match[1]+'\"]');\n"
             "        var col=colInput.value;\n"
             "        var val=valInput?valInput.value:'';\n"
             "        if(col&&val){pairs.push({col:col,val:val});}\n"
             "      });\n"
             "      return pairs;\n"
             "    }\n"
             "    function buildInsertPreview(form){\n"
             "      var db=getWriteDatabase();\n"
             "      var table=getWriteTable();\n"
             "      if(!isSafeWriteIdentifier(db)||!isSafeWriteIdentifier(table)){return '';}\n"
             "      var pairs=collectWritePairs(form);\n"
             "      if(!pairs.length){return '';}\n"
             "      var cols=[];\n"
             "      var vals=[];\n"
             "      pairs.forEach(function(pair){\n"
             "        var ident=sqlWriteIdent(pair.col);\n"
             "        if(!ident){return;}\n"
             "        cols.push(ident);\n"
             "        vals.push(sqlLiteral(pair.val));\n"
             "      });\n"
             "      if(!cols.length){return '';}\n"
             "      return 'INSERT INTO ['+db+'].[dbo].['+table+'] ('+cols.join(',')+') VALUES ('+vals.join(',')+');';\n"
             "    }\n"
             "    function buildUpdatePreview(form){\n"
             "      var db=getWriteDatabase();\n"
             "      var table=getWriteTable();\n"
             "      if(!form||!isSafeWriteIdentifier(db)||!isSafeWriteIdentifier(table)){return '';}\n"
             "      var whereColEl=form.querySelector('[name=\"where_column\"]');\n"
             "      var whereValEl=form.querySelector('[name=\"where_value\"]');\n"
             "      var whereCol=whereColEl?whereColEl.value:'';\n"
             "      var whereVal=whereValEl?whereValEl.value:'';\n"
             "      if(!isSafeWriteIdentifier(whereCol)||!whereVal){return '';}\n"
             "      var sets=[];\n"
             "      collectWritePairs(form).forEach(function(pair){\n"
             "        if(pair.col===whereCol){return;}\n"
             "        var ident=sqlWriteIdent(pair.col);\n"
             "        if(!ident){return;}\n"
             "        sets.push(ident+'='+sqlLiteral(pair.val));\n"
             "      });\n"
             "      if(!sets.length){return '';}\n"
             "      return 'UPDATE ['+db+'].[dbo].['+table+'] SET '+sets.join(',')+' WHERE ['+whereCol+']='+sqlLiteral(whereVal)+';';\n"
             "    }\n"
             "    function getActiveWritePanel(){\n"
             "      var customPanel=byId('write-custom-panel');\n"
             "      if(customPanel&&customPanel.open){return 'custom';}\n"
             "      var updatePanel=byId('write-update-panel');\n"
             "      if(updatePanel&&updatePanel.open){return 'update';}\n"
             "      var insertPanel=byId('write-insert-panel');\n"
             "      if(insertPanel&&insertPanel.open){return 'insert';}\n"
             "      return '';\n"
             "    }\n"
             "    function updateWritePreview(){\n"
             "      var mode=getActiveWritePanel();\n"
             "      if(!mode){\n"
             "        if(writePreviewLabel){writePreviewLabel.textContent='SQL Preview';}\n"
             "        if(writePreviewHint){writePreviewHint.textContent='Expand a section above and fill fields to preview generated SQL here before submitting.';}\n"
             "        writePreviewEl.value=stagedWritePreview;\n"
             "        return;\n"
             "      }\n"
             "      var built='';\n"
             "      if(mode==='custom'){\n"
             "        var customForm=byId('write-sql-form');\n"
             "        var customSql=customForm?customForm.querySelector('[name=\"write_sql\"]'):null;\n"
             "        built=customSql?customSql.value:'';\n"
             "        if(writePreviewLabel){writePreviewLabel.textContent='Custom Write SQL';}\n"
             "        if(writePreviewHint){writePreviewHint.textContent='Preview mirrors your custom SQL. Click Preview Custom Write when ready.';}\n"
             "      }else if(mode==='update'){\n"
             "        built=buildUpdatePreview(byId('update-form'));\n"
             "        if(writePreviewLabel){writePreviewLabel.textContent='Generated UPDATE';}\n"
             "        if(writePreviewHint){writePreviewHint.textContent='WHERE column is never updated. Blank value fields are ignored.';}\n"
             "      }else{\n"
             "        built=buildInsertPreview(byId('insert-form'));\n"
             "        if(writePreviewLabel){writePreviewLabel.textContent='Generated INSERT';}\n"
             "        if(writePreviewHint){writePreviewHint.textContent='Only non-empty column values are included in the preview.';}\n"
             "      }\n"
             "      writePreviewEl.value=built||stagedWritePreview;\n"
             "    }\n"
             "    function syncConnectedTable(){\n"
             "      var hidden=byId('connected-table');\n"
             "      if(hidden){hidden.value=getWriteTable();}\n"
             "    }\n"
             "    function clearStagedWritePreview(){\n"
             "      stagedWritePreview='';\n"
             "      syncConnectedTable();\n"
             "      updateWritePreview();\n"
             "    }\n"
             "    ['write-insert-panel','write-update-panel','write-custom-panel'].forEach(function(id){\n"
             "      var panel=byId(id);\n"
             "      if(!panel){return;}\n"
             "      panel.addEventListener('toggle',updateWritePreview);\n"
             "    });\n"
             "    ['insert-form','update-form','write-sql-form'].forEach(function(id){\n"
             "      var form=byId(id);\n"
             "      if(!form){return;}\n"
             "      form.addEventListener('input',clearStagedWritePreview);\n"
             "      form.addEventListener('change',clearStagedWritePreview);\n"
             "    });\n"
             "    var writeTableField=byId('table-field');\n"
             "    if(writeTableField){\n"
             "      writeTableField.addEventListener('change',clearStagedWritePreview);\n"
             "    }\n"
             "    updateWritePreview();\n"
             "  }\n"
             "})();\n"
             "</script>\n");
}

void ShowAutoRefresh(int sock, PCSTR url)
{
    if (SqlRuntimeIsBusy()) {
        fdprintf(sock, "<meta http-equiv=\"refresh\" content=\"1;url=");
        HtmlWriteEscaped(sock, (url != nullptr && url[0] != '\0') ? url : "index.html");
        fdprintf(sock, "\">");
    }
}

void ShowWritePageInit(int sock, PCSTR /*url*/)
{
    if (SqlRuntimeIsBusy()) {
        return;
    }

    const sql_runtime_config & cfg = SqlRuntimeGetConfig();
    if (cfg.database[0] == '\0') {
        return;
    }

    const sql_table_catalog & tables = SqlCatalogGet();
    if (!tables.ready || !tables.success || !SqlCatalogTablesMatchDatabase(cfg.database)) {
        SqlRuntimeRequestBrowseTables();
        return;
    }

    if (cfg.table[0] == '\0') {
        return;
    }

    const sql_column_catalog & catalog = SqlColumnCatalogGet();
    if (catalog.ready && catalog.success &&
        SqlColumnCatalogMatches(cfg.database, cfg.table)) {
        return;
    }

    SqlRuntimeRequestBrowseColumns();
}

void ShowQueryStatus(int sock, PCSTR /*url*/)
{
    if (SqlRuntimeIsTestingConnection()) {
        fdprintf(sock, "Testing SQL Server connection...");
        return;
    }

    if (SqlRuntimeIsRunningQuery()) {
        fdprintf(sock, "Running query...");
        return;
    }

    if (SqlRuntimeIsExecutingMutation()) {
        fdprintf(sock, "Gateway writing to SQL...");
        return;
    }

    if (SqlRuntimeIsBrowsingColumns()) {
        fdprintf(sock, "Loading column list...");
        return;
    }

    if (SqlRuntimeIsBrowsingTables()) {
        fdprintf(sock, "Loading table list...");
        return;
    }

    if (!SqlRuntimeIsConnectionVerified()) {
        fdprintf(sock, "Step 1: connect to SQL Server, then browse and query in step 2.");
        return;
    }

    const sql_result_store & store = SqlResultsGet();
    if (!store.ready) {
        fdprintf(sock, "Connected — choose a table and run a query.");
        return;
    }

    if (!store.success) {
        fdprintf(sock, "Last query failed — see results in step 3.");
        return;
    }

    fdprintf(sock, "Last query returned %lu row(s).",
             static_cast<unsigned long>(store.row_count));
}

void ShowPageEyebrow(int sock, PCSTR /*url*/)
{
    const sql_result_store & store = SqlResultsGet();
    if (store.database[0] != '\0') {
        HtmlWriteEscaped(sock, store.database);
        return;
    }

    const char * database = SqlRuntimeGetConfig().database;
    if (database[0] != '\0') {
        HtmlWriteEscaped(sock, database);
    }
}

void ShowPageHeading(int sock, PCSTR /*url*/)
{
    const sql_result_store & store = SqlResultsGet();
    if (store.page_heading[0] != '\0') {
        HtmlWriteEscaped(sock, store.page_heading);
        return;
    }
    HtmlWriteEscaped(sock, "SQL Query Console");
}

void ShowTableTitle(int sock, PCSTR /*url*/)
{
    const sql_result_store & store = SqlResultsGet();
    HtmlWriteEscaped(sock, store.table_label[0] != '\0' ? store.table_label : "Results");
}

void ShowTrayTableHeader(int sock, PCSTR /*url*/)
{
    const sql_result_store & store = SqlResultsGet();

    if (!store.ready || !store.success || store.col_count == 0) {
        fdprintf(sock, "<tr><th>Status</th></tr>");
        return;
    }

    fdprintf(sock, "<tr><th class=\"nb-row-num\">#</th>");
    for (tdsl::uint32_t i = 0; i < store.col_count; ++i) {
        fdprintf(sock, "<th>");
        HtmlWriteEscaped(sock, store.col_names[i]);
        fdprintf(sock, "</th>");
    }
    fdprintf(sock, "</tr>\r\n");
}

void ShowTrayTableRows(int sock, PCSTR /*url*/)
{
    const sql_result_store & store = SqlResultsGet();

    if (SqlRuntimeIsBusy()) {
        fdprintf(sock, "<tr><td>Running query...</td></tr>\r\n");
        return;
    }

    if (!store.ready) {
        fdprintf(sock, "<tr><td>No results yet.</td></tr>\r\n");
        return;
    }

    if (!store.success) {
        fdprintf(sock, "<tr><td>");
        HtmlWriteEscaped(sock, store.status);
        fdprintf(sock, "</td></tr>\r\n");
        return;
    }

    if (store.row_count == 0) {
        fdprintf(sock, "<tr><td colspan=\"%lu\">No rows returned.</td></tr>\r\n",
                 static_cast<unsigned long>(store.col_count > 0 ? store.col_count + 1 : 1));
        return;
    }

    for (tdsl::uint32_t row = 0; row < store.row_count; ++row) {
        fdprintf(sock, "<tr>");
        fdprintf(sock, "<td class=\"nb-row-num\">%lu</td>",
                 static_cast<unsigned long>(row + 1));
        for (tdsl::uint32_t col = 0; col < store.col_count; ++col) {
            fdprintf(sock, "<td class=\"nb-spec\">");
            HtmlWriteResultCell(sock, store.cells[row][col]);
            fdprintf(sock, "</td>");
        }
        fdprintf(sock, "</tr>\r\n");
    }
}

void ShowCsvExport(int sock, PCSTR /*url*/)
{
    const sql_result_store & store = SqlResultsGet();

    if (!store.ready) {
        fdprintf(sock, "status\r\nNo results yet\r\n");
        return;
    }

    if (!store.success) {
        fdprintf(sock, "status\r\n");
        CsvWriteEscaped(sock, store.status);
        fdprintf(sock, "\r\n");
        return;
    }

    fdprintf(sock, "#");
    for (tdsl::uint32_t col = 0; col < store.col_count; ++col) {
        fdprintf(sock, ",");
        CsvWriteEscaped(sock, store.col_names[col]);
    }
    fdprintf(sock, "\r\n");

    for (tdsl::uint32_t row = 0; row < store.row_count; ++row) {
        fdprintf(sock, "%lu", static_cast<unsigned long>(row + 1));
        for (tdsl::uint32_t col = 0; col < store.col_count; ++col) {
            fdprintf(sock, ",");
            CsvWriteEscaped(sock, store.cells[row][col]);
        }
        fdprintf(sock, "\r\n");
    }
}

void ShowDiagnosticsPanel(int sock, PCSTR /*url*/)
{
    const sql_runtime_config & cfg = SqlRuntimeGetConfig();
    const sql_result_store & store = SqlResultsGet();
    const sql_connection_status & conn = SqlConnectionStatusGet();

    fdprintf(sock, "<details class=\"nb-diagnostics\"><summary>View Diagnostics</summary>");
    fdprintf(sock, "<div class=\"nb-diagnostics-grid\">");

    fdprintf(sock, "<section><h4>Connection</h4><dl>");
    fdprintf(sock, "<dt>Server</dt><dd>");
    HtmlWriteEscaped(sock, cfg.server[0] != '\0' ? cfg.server : "not set");
    fdprintf(sock, ":%u</dd>", static_cast<unsigned>(cfg.port != 0 ? cfg.port : SQL_DEFAULT_PORT));
    fdprintf(sock, "<dt>Database</dt><dd>");
    HtmlWriteEscaped(sock, cfg.database[0] != '\0' ? cfg.database : "not set");
    fdprintf(sock, "</dd><dt>User</dt><dd>");
    HtmlWriteEscaped(sock, cfg.user[0] != '\0' ? cfg.user : "not set");
    fdprintf(sock, "</dd><dt>Status</dt><dd>");
    if (conn.ready) {
        HtmlWriteEscaped(sock, conn.message);
    }
    else {
        fdprintf(sock, "No connection test in this session.");
    }
    fdprintf(sock, "</dd><dt>Saved settings</dt><dd>%s</dd></dl></section>",
             SqlNvHasSaved() ? "present" : "not saved");

    fdprintf(sock, "<section><h4>Query</h4><dl>");
    fdprintf(sock, "<dt>Mode</dt><dd>%s</dd>", cfg.use_table_builder ? "Builder" : "Custom SQL");
    fdprintf(sock, "<dt>Maximum rows</dt><dd>%lu / hard cap %u</dd>",
             static_cast<unsigned long>(cfg.top_n != 0 ? cfg.top_n : SQL_DEFAULT_TOP_N),
             SQL_MAX_TOP_N);
    fdprintf(sock, "<dt>Result</dt><dd>");
    if (store.ready) {
        fdprintf(sock, "%s, %lu row(s), %lu column(s), %lu ms",
                 store.success ? "success" : "failed",
                 static_cast<unsigned long>(store.row_count),
                 static_cast<unsigned long>(store.col_count),
                 static_cast<unsigned long>(store.query_duration_ms));
    }
    else {
        fdprintf(sock, "No query has completed.");
    }
    fdprintf(sock, "</dd><dt>Console</dt><dd><a href=\"console.html\">Remote console</a></dd>");
    fdprintf(sock, "</dl></section>");

    fdprintf(sock, "</div></details>");
}

static void ApplyFormPostStart()
{
    g_post_config            = SqlRuntimeGetConfig();
    g_post_use_table_builder = false;
    g_post_password_provided = false;
    g_post_columns_seen      = false;
    g_post_return_to[0]      = '\0';
}

static void RedirectAfterBrowse(int sock, const char * default_page)
{
    if (g_post_return_to[0] != '\0') {
        RedirectResponse(sock, g_post_return_to);
        return;
    }

    RedirectResponse(sock, default_page);
}

static void AppendPostColumn(const char * value)
{
    if (!g_post_columns_seen) {
        g_post_config.columns[0] = '\0';
        g_post_columns_seen      = true;
    }

    if (value == nullptr || value[0] == '\0') {
        return;
    }

    const tdsl::size_t used = strlen(g_post_config.columns);
    const tdsl::size_t need = strlen(value) + (used > 0 ? 1 : 0);
    if (used + need + 1 >= sizeof(g_post_config.columns)) {
        return;
    }

    if (used > 0) {
        strncat(g_post_config.columns, ",", sizeof(g_post_config.columns) - used - 1);
    }
    strncat(g_post_config.columns, value,
            sizeof(g_post_config.columns) - strlen(g_post_config.columns) - 1);
}

static void ApplyFormPostField(const char * pName, const char * pValue)
{
    if (pName == nullptr) {
        return;
    }

    if (strcmp(pName, "server") == 0) {
        CopyField(g_post_config.server, sizeof(g_post_config.server), pValue);
    }
    else if (strcmp(pName, "port") == 0) {
        if (pValue != nullptr && pValue[0] != '\0') {
            const long port = strtol(pValue, nullptr, 10);
            if (port > 0 && port <= 65535) {
                g_post_config.port = static_cast<tdsl::uint16_t>(port);
            }
        }
        else {
            g_post_config.port = SQL_DEFAULT_PORT;
        }
    }
    else if (strcmp(pName, "user") == 0) {
        CopyField(g_post_config.user, sizeof(g_post_config.user), pValue);
    }
    else if (strcmp(pName, "password") == 0) {
        if (pValue != nullptr && pValue[0] != '\0') {
            CopyField(g_post_config.password, sizeof(g_post_config.password), pValue);
            g_post_password_provided = true;
        }
    }
    else if (strcmp(pName, "database") == 0) {
        if (pValue != nullptr && strcmp(pValue, "__manual__") != 0) {
            CopyField(g_post_config.database, sizeof(g_post_config.database), pValue);
        }
    }
    else if (strcmp(pName, "database_manual") == 0) {
        if (pValue != nullptr && pValue[0] != '\0') {
            CopyField(g_post_config.database, sizeof(g_post_config.database), pValue);
        }
    }
    else if (strcmp(pName, "table") == 0) {
        if (pValue != nullptr && strcmp(pValue, "__manual__") != 0) {
            CopyField(g_post_config.table, sizeof(g_post_config.table), pValue);
        }
    }
    else if (strcmp(pName, "table_manual") == 0) {
        if (pValue != nullptr && pValue[0] != '\0') {
            CopyField(g_post_config.table, sizeof(g_post_config.table), pValue);
        }
    }
    else if (strcmp(pName, "columns") == 0) {
        AppendPostColumn(pValue);
    }
    else if (strcmp(pName, "top_n") == 0) {
        if (pValue != nullptr && pValue[0] != '\0') {
            const long top_n = strtol(pValue, nullptr, 10);
            if (top_n > 0 && top_n <= SQL_MAX_TOP_N) {
                g_post_config.top_n = static_cast<tdsl::uint32_t>(top_n);
            }
        }
        else {
            g_post_config.top_n = SQL_DEFAULT_TOP_N;
        }
    }
    else if (strcmp(pName, "filter_column") == 0) {
        CopyField(g_post_config.filter_column, sizeof(g_post_config.filter_column), pValue);
    }
    else if (strcmp(pName, "filter_op") == 0) {
        CopyField(g_post_config.filter_op, sizeof(g_post_config.filter_op), pValue);
    }
    else if (strcmp(pName, "filter_value") == 0) {
        CopyField(g_post_config.filter_value, sizeof(g_post_config.filter_value), pValue);
    }
    else if (strcmp(pName, "sort_column") == 0) {
        CopyField(g_post_config.sort_column, sizeof(g_post_config.sort_column), pValue);
    }
    else if (strcmp(pName, "sort_desc") == 0) {
        g_post_config.sort_desc = (pValue != nullptr && strcmp(pValue, "desc") == 0);
    }
    else if (strcmp(pName, "query") == 0) {
        CopyField(g_post_config.query, sizeof(g_post_config.query), pValue);
    }
    else if (strcmp(pName, "use_table_builder") == 0) {
        g_post_use_table_builder = (pValue != nullptr && strcmp(pValue, "on") == 0);
    }
    else if (strcmp(pName, "return_to") == 0) {
        if (pValue != nullptr &&
            (strcmp(pValue, "write.html") == 0 || strcmp(pValue, "browse.html") == 0)) {
            CopyField(g_post_return_to, sizeof(g_post_return_to), pValue);
        }
    }
}

static void ApplyFormPostPassword()
{
    if (!g_post_password_provided) {
        const sql_runtime_config & current = SqlRuntimeGetConfig();
        CopyField(g_post_config.password, sizeof(g_post_config.password), current.password);
    }
}

static void ApplyWritePostStart()
{
    ApplyFormPostStart();
    memset(g_write_sql, 0, sizeof(g_write_sql));
    memset(g_write_where_column, 0, sizeof(g_write_where_column));
    memset(g_write_where_value, 0, sizeof(g_write_where_value));
    memset(g_write_cols, 0, sizeof(g_write_cols));
    memset(g_write_vals, 0, sizeof(g_write_vals));
    memset(g_write_use, 0, sizeof(g_write_use));
}

static int ParseIndexedField(const char * name, const char * prefix)
{
    if (name == nullptr || prefix == nullptr) {
        return -1;
    }

    const tdsl::size_t prefix_len = strlen(prefix);
    if (strncmp(name, prefix, prefix_len) != 0) {
        return -1;
    }

    const long index = strtol(name + prefix_len, nullptr, 10);
    if (index < 0 || index >= SQL_CATALOG_MAX_COLUMNS) {
        return -1;
    }
    return static_cast<int>(index);
}

static void ApplyWritePostField(const char * pName, const char * pValue)
{
    ApplyFormPostField(pName, pValue);

    if (pName == nullptr) {
        return;
    }

    if (strcmp(pName, "where_column") == 0) {
        CopyField(g_write_where_column, sizeof(g_write_where_column), pValue);
    }
    else if (strcmp(pName, "where_value") == 0) {
        CopyField(g_write_where_value, sizeof(g_write_where_value), pValue);
    }
    else if (strcmp(pName, "write_sql") == 0) {
        CopyField(g_write_sql, sizeof(g_write_sql), pValue);
    }
    else {
        int index = ParseIndexedField(pName, "col");
        if (index >= 0) {
            CopyField(g_write_cols[index], sizeof(g_write_cols[index]), pValue);
            return;
        }

        index = ParseIndexedField(pName, "val");
        if (index >= 0) {
            CopyField(g_write_vals[index], sizeof(g_write_vals[index]), pValue);
            return;
        }

        index = ParseIndexedField(pName, "use");
        if (index >= 0) {
            g_write_use[index] = (pValue != nullptr && strcmp(pValue, "on") == 0);
            return;
        }
    }
}

static bool BuildGuidedInsertSql(char * dst, tdsl::size_t cap)
{
    const sql_runtime_config & cfg = g_post_config;
    if (dst == nullptr || cap == 0 || cfg.database[0] == '\0' || cfg.table[0] == '\0' ||
        !IsSafeSqlIdentifierForWeb(cfg.table)) {
        return false;
    }

    dst[0] = '\0';
    if (!AppendSql(dst, cap, "INSERT INTO ") ||
        !AppendSqlIdentifier(dst, cap, cfg.database) ||
        !AppendSql(dst, cap, ".[dbo].") ||
        !AppendSqlIdentifier(dst, cap, cfg.table) ||
        !AppendSql(dst, cap, " (")) {
        return false;
    }

    bool any = false;
    for (tdsl::uint32_t i = 0; i < SQL_CATALOG_MAX_COLUMNS; ++i) {
        if (g_write_cols[i][0] == '\0' || g_write_vals[i][0] == '\0') {
            continue;
        }
        if (any && !AppendSql(dst, cap, ",")) {
            return false;
        }
        if (!AppendSqlIdentifier(dst, cap, g_write_cols[i])) {
            return false;
        }
        any = true;
    }

    if (!any || !AppendSql(dst, cap, ") VALUES (")) {
        return false;
    }

    any = false;
    for (tdsl::uint32_t i = 0; i < SQL_CATALOG_MAX_COLUMNS; ++i) {
        if (g_write_cols[i][0] == '\0' || g_write_vals[i][0] == '\0') {
            continue;
        }
        char literal[SQL_CFG_FILTER_LEN * 2] = {};
        if (!WriteSqlLiteralForWeb(literal, sizeof(literal), g_write_vals[i])) {
            return false;
        }
        if (any && !AppendSql(dst, cap, ",")) {
            return false;
        }
        if (!AppendSql(dst, cap, literal)) {
            return false;
        }
        any = true;
    }

    return AppendSql(dst, cap, ");");
}

static bool BuildGuidedUpdateSql(char * dst, tdsl::size_t cap)
{
    const sql_runtime_config & cfg = g_post_config;
    if (dst == nullptr || cap == 0 || cfg.database[0] == '\0' || cfg.table[0] == '\0' ||
        !IsSafeSqlIdentifierForWeb(cfg.table) || !IsSafeSqlIdentifierForWeb(g_write_where_column) ||
        g_write_where_value[0] == '\0') {
        return false;
    }

    dst[0] = '\0';
    if (!AppendSql(dst, cap, "UPDATE ") ||
        !AppendSqlIdentifier(dst, cap, cfg.database) ||
        !AppendSql(dst, cap, ".[dbo].") ||
        !AppendSqlIdentifier(dst, cap, cfg.table) ||
        !AppendSql(dst, cap, " SET ")) {
        return false;
    }

    bool any = false;
    for (tdsl::uint32_t i = 0; i < SQL_CATALOG_MAX_COLUMNS; ++i) {
        if (g_write_cols[i][0] == '\0' || g_write_vals[i][0] == '\0') {
            continue;
        }

        if (strcmp(g_write_cols[i], g_write_where_column) == 0) {
            continue;
        }

        char literal[SQL_CFG_FILTER_LEN * 2] = {};
        if (!WriteSqlLiteralForWeb(literal, sizeof(literal), g_write_vals[i])) {
            return false;
        }

        if (any && !AppendSql(dst, cap, ",")) {
            return false;
        }
        if (!AppendSqlIdentifier(dst, cap, g_write_cols[i]) ||
            !AppendSql(dst, cap, "=") ||
            !AppendSql(dst, cap, literal)) {
            return false;
        }
        any = true;
    }

    if (!any || !AppendSql(dst, cap, " WHERE ") ||
        !AppendSqlIdentifier(dst, cap, g_write_where_column) ||
        !AppendSql(dst, cap, "=")) {
        return false;
    }

    char where_literal[SQL_CFG_FILTER_LEN * 2] = {};
    if (!WriteSqlLiteralForWeb(where_literal, sizeof(where_literal), g_write_where_value)) {
        return false;
    }

    return AppendSql(dst, cap, where_literal) && AppendSql(dst, cap, ";");
}

static void StageMutationOrResult(sql_mutation_kind kind, const char * sql, const char * summary)
{
    sql_result_store & store = SqlResultsGet();
    SqlResultsReset();

    if (!SqlRuntimeStageMutation(kind, sql, summary)) {
        store.ready   = true;
        store.success = false;
        strncpy(store.status, "Unable to stage write operation.", sizeof(store.status) - 1);
        return;
    }

    store.ready   = true;
    store.success = true;
    strncpy(store.status, "Write preview staged. Confirm before execution.",
            sizeof(store.status) - 1);
}

static void RunQueryPostCallback(int sock, PostEvents event, const char *pName,
                                 const char *pValue)
{
    switch (event) {
        case eStartingPost:
            ApplyFormPostStart();
            break;

        case eVariable:
            ApplyFormPostField(pName, pValue);
            break;

        case eEndOfPost:
            ApplyFormPostPassword();

            g_post_config.use_table_builder = g_post_use_table_builder;

            if (g_post_config.use_table_builder) {
                if (!SqlRuntimeBuildTableQuery(g_post_config.query, sizeof(g_post_config.query),
                                               g_post_config)) {
                    RedirectResponse(sock, "browse.html");
                    break;
                }
            }
            else if (!IsReadOnlySql(g_post_config.query)) {
                sql_result_store & store = SqlResultsGet();
                SqlResultsReset();
                store.ready   = true;
                store.success = false;
                strncpy(store.status,
                        "Custom SQL is read-only in this build. Use a SELECT statement.",
                        sizeof(store.status) - 1);
                RedirectResponse(sock, "results.html");
                break;
            }

            if (SqlRuntimeIsBusy()) {
                RedirectResponse(sock, "results.html");
                break;
            }

            SqlRuntimeUpdateConfig(g_post_config);
            SqlRuntimeRequestRun();
            RedirectResponse(sock, "results.html");
            break;

        default:
            break;
    }
}

HtmlPostVariableListCallback g_run_query_post("runquery*", RunQueryPostCallback);

static void PreviewInsertPostCallback(int sock, PostEvents event, const char * pName,
                                      const char * pValue)
{
    switch (event) {
        case eStartingPost:
            ApplyWritePostStart();
            break;

        case eVariable:
            ApplyWritePostField(pName, pValue);
            break;

        case eEndOfPost: {
            ApplyFormPostPassword();
            SqlRuntimeUpdateConfig(g_post_config);

            char sql[SQL_CFG_QUERY_LEN] = {};
            if (BuildGuidedInsertSql(sql, sizeof(sql))) {
                char summary[SQL_MUTATION_SUMMARY_LEN] = {};
                snprintf(summary, sizeof(summary), "Insert row into %s.", g_post_config.table);
                StageMutationOrResult(SQL_MUTATION_INSERT, sql, summary);
                RedirectResponse(sock, "write.html");
            }
            else {
                sql_result_store & store = SqlResultsGet();
                SqlResultsReset();
                store.ready   = true;
                store.success = false;
                strncpy(store.status, "Invalid insert values. Enter at least one column value.",
                        sizeof(store.status) - 1);
                RedirectResponse(sock, "write.html");
            }
            break;
        }

        default:
            break;
    }
}

HtmlPostVariableListCallback g_preview_insert_post("previewinsert*", PreviewInsertPostCallback);

static void PreviewUpdatePostCallback(int sock, PostEvents event, const char * pName,
                                      const char * pValue)
{
    switch (event) {
        case eStartingPost:
            ApplyWritePostStart();
            break;

        case eVariable:
            ApplyWritePostField(pName, pValue);
            break;

        case eEndOfPost: {
            ApplyFormPostPassword();
            SqlRuntimeUpdateConfig(g_post_config);

            char sql[SQL_CFG_QUERY_LEN] = {};
            if (BuildGuidedUpdateSql(sql, sizeof(sql))) {
                char summary[SQL_MUTATION_SUMMARY_LEN] = {};
                snprintf(summary, sizeof(summary), "Update row(s) in %s where %s matches.",
                         g_post_config.table, g_write_where_column);
                StageMutationOrResult(SQL_MUTATION_UPDATE, sql, summary);
                RedirectResponse(sock, "write.html");
            }
            else {
                sql_result_store & store = SqlResultsGet();
                SqlResultsReset();
                store.ready   = true;
                store.success = false;
                strncpy(store.status,
                        "Invalid update. Provide a WHERE key and at least one non-key value to change.",
                        sizeof(store.status) - 1);
                RedirectResponse(sock, "write.html");
            }
            break;
        }

        default:
            break;
    }
}

HtmlPostVariableListCallback g_preview_update_post("previewupdate*", PreviewUpdatePostCallback);

static void PreviewWriteSqlPostCallback(int sock, PostEvents event, const char * pName,
                                        const char * pValue)
{
    switch (event) {
        case eStartingPost:
            ApplyWritePostStart();
            break;

        case eVariable:
            ApplyWritePostField(pName, pValue);
            break;

        case eEndOfPost: {
            ApplyFormPostPassword();
            SqlRuntimeUpdateConfig(g_post_config);

            if (IsWriteSqlAllowed(g_write_sql)) {
                StageMutationOrResult(SQL_MUTATION_CUSTOM, g_write_sql,
                                      "Execute custom write SQL.");
                RedirectResponse(sock, "write.html");
            }
            else {
                sql_result_store & store = SqlResultsGet();
                SqlResultsReset();
                store.ready   = true;
                store.success = false;
                strncpy(store.status,
                        "Custom write SQL must start with INSERT or UPDATE.",
                        sizeof(store.status) - 1);
                RedirectResponse(sock, "write.html");
            }
            break;
        }

        default:
            break;
    }
}

HtmlPostVariableListCallback g_preview_write_sql_post("previewwritesql*", PreviewWriteSqlPostCallback);

static void ConfirmMutationPostCallback(int sock, PostEvents event, const char * /*pName*/,
                                        const char * /*pValue*/)
{
    if (event != eEndOfPost) {
        return;
    }

    if (!SqlRuntimeIsBusy() && SqlRuntimeGetPendingMutation().valid) {
        SqlRuntimeRequestExecutePendingMutation();
    }
    RedirectResponse(sock, "results.html");
}

HtmlPostVariableListCallback g_confirm_mutation_post("confirmmutation*", ConfirmMutationPostCallback);

static void CancelMutationPostCallback(int sock, PostEvents event, const char * /*pName*/,
                                       const char * /*pValue*/)
{
    if (event != eEndOfPost) {
        return;
    }

    SqlRuntimeClearPendingMutation();
    RedirectResponse(sock, "write.html");
}

HtmlPostVariableListCallback g_cancel_mutation_post("cancelmutation*", CancelMutationPostCallback);

static void SetWriteTablePostCallback(int sock, PostEvents event, const char * pName,
                                      const char * pValue)
{
    switch (event) {
        case eStartingPost:
            ApplyFormPostStart();
            break;

        case eVariable:
            ApplyFormPostField(pName, pValue);
            break;

        case eEndOfPost:
            ApplyFormPostPassword();

            if (SqlRuntimeIsBusy()) {
                RedirectResponse(sock, "write.html");
                break;
            }

            SqlRuntimeUpdateConfig(g_post_config);

            if (g_post_config.table[0] == '\0') {
                sql_result_store & store = SqlResultsGet();
                SqlResultsReset();
                store.ready   = true;
                store.success = false;
                strncpy(store.status, "Choose a table before loading columns.",
                        sizeof(store.status) - 1);
                RedirectResponse(sock, "write.html");
                break;
            }

            SqlRuntimeRequestBrowseColumns();
            RedirectResponse(sock, "write.html");
            break;

        default:
            break;
    }
}

HtmlPostVariableListCallback g_set_write_table_post("setwritetable*", SetWriteTablePostCallback);

static void BrowseTablesPostCallback(int sock, PostEvents event, const char * pName,
                                     const char * pValue)
{
    switch (event) {
        case eStartingPost:
            ApplyFormPostStart();
            break;

        case eVariable:
            ApplyFormPostField(pName, pValue);
            break;

        case eEndOfPost:
            ApplyFormPostPassword();

            if (SqlRuntimeIsBusy()) {
                RedirectAfterBrowse(sock, "browse.html");
                break;
            }

            SqlRuntimeUpdateConfig(g_post_config);
            SqlRuntimeRequestBrowseTables();
            RedirectAfterBrowse(sock, "browse.html");
            break;

        default:
            break;
    }
}

HtmlPostVariableListCallback g_browse_tables_post("browsetables*", BrowseTablesPostCallback);

static void BrowseColumnsPostCallback(int sock, PostEvents event, const char * pName,
                                      const char * pValue)
{
    switch (event) {
        case eStartingPost:
            ApplyFormPostStart();
            break;

        case eVariable:
            ApplyFormPostField(pName, pValue);
            break;

        case eEndOfPost:
            ApplyFormPostPassword();

            if (SqlRuntimeIsBusy()) {
                RedirectAfterBrowse(sock, "browse.html");
                break;
            }

            SqlRuntimeUpdateConfig(g_post_config);
            SqlRuntimeRequestBrowseColumns();
            RedirectAfterBrowse(sock, "browse.html");
            break;

        default:
            break;
    }
}

HtmlPostVariableListCallback g_browse_columns_post("browsecolumns*", BrowseColumnsPostCallback);

static void BrowseDatabasesPostCallback(int sock, PostEvents event, const char * pName,
                                        const char * pValue)
{
    switch (event) {
        case eStartingPost:
            ApplyFormPostStart();
            break;

        case eVariable:
            ApplyFormPostField(pName, pValue);
            break;

        case eEndOfPost:
            ApplyFormPostPassword();

            if (SqlRuntimeIsBusy()) {
                RedirectResponse(sock, "browse.html");
                break;
            }

            SqlRuntimeUpdateConfig(g_post_config);
            SqlRuntimeRequestBrowseDatabases();
            RedirectResponse(sock, "browse.html");
            break;

        default:
            break;
    }
}

HtmlPostVariableListCallback g_browse_databases_post("browsedatabases*", BrowseDatabasesPostCallback);

void ShowConnectionTestStatus(int sock, PCSTR /*url*/)
{
    if (SqlRuntimeIsTestingConnection()) {
        fdprintf(sock, "Testing connection...");
        return;
    }

    const sql_connection_status & status = SqlConnectionStatusGet();

    if (status.ready) {
        if (status.success) {
            fdprintf(sock, "<span class=\"nb-status nb-status-ok\">");
        }
        else {
            fdprintf(sock, "<span class=\"nb-status nb-status-err\">");
        }
        HtmlWriteEscaped(sock, status.message);
        fdprintf(sock, "</span>");
        return;
    }

    if (SqlNvHasSaved()) {
        fdprintf(sock, "Saved settings loaded from NV flash. Click Test Connection to verify.");
        return;
    }

    fdprintf(sock, "Enter server details and click Test Connection.");
}

static void TestConnectionPostCallback(int sock, PostEvents event, const char * pName,
                                       const char * pValue)
{
    switch (event) {
        case eStartingPost:
            ApplyFormPostStart();
            break;

        case eVariable:
            ApplyFormPostField(pName, pValue);
            break;

        case eEndOfPost:
            ApplyFormPostPassword();

            if (SqlRuntimeIsBusy()) {
                RedirectResponse(sock, "configure.html");
                break;
            }

            SqlRuntimeUpdateConfig(g_post_config);
            SqlRuntimeRequestTestConnection();
            RedirectResponse(sock, "configure.html");
            break;

        default:
            break;
    }
}

HtmlPostVariableListCallback g_test_connection_post("testconnection*", TestConnectionPostCallback);

void ShowSaveSettingsButton(int sock, PCSTR /*url*/)
{
    const sql_connection_status & status = SqlConnectionStatusGet();
    if (!SqlRuntimeIsConnectionVerified() || !status.ready || !status.success) {
        fdprintf(sock,
                 "<button type=\"submit\" class=\"nb-btn nb-btn-secondary\" "
                 "formaction=\"savesettings\" formmethod=\"post\" disabled>Save Settings</button>");
        return;
    }

    fdprintf(sock,
             "<button type=\"submit\" class=\"nb-btn nb-btn-secondary\" "
             "formaction=\"savesettings\" formmethod=\"post\" "
             "title=\"Write validated settings to NV flash\">Save Settings</button>");
}

static void SaveSettingsPostCallback(int sock, PostEvents event, const char * /*pName*/,
                                     const char * /*pValue*/)
{
    if (event != eEndOfPost) {
        return;
    }

    if (!SqlRuntimeIsBusy()) {
        SqlRuntimeSaveCurrentConfig();
    }

    RedirectResponse(sock, "configure.html");
}

HtmlPostVariableListCallback g_save_settings_post("savesettings*", SaveSettingsPostCallback);

void ShowClearNvButton(int sock, PCSTR /*url*/)
{
    if (!SqlNvHasSaved()) {
        return;
    }

    fdprintf(sock,
             "<details class=\"nb-danger-zone\"><summary>Saved settings</summary>"
             "<p class=\"nb-field-hint\">Clear removes SQL settings and the stored password from NV flash.</p>"
             "<button type=\"submit\" class=\"nb-btn nb-btn-danger\" "
             "formaction=\"clearnv\" formmethod=\"post\" formnovalidate "
             "title=\"Erase all saved settings from NV flash\">Clear Saved Settings</button>"
             "</details>");
}

static void ClearNvPostCallback(int sock, PostEvents event, const char * /*pName*/,
                                const char * /*pValue*/)
{
    if (event != eEndOfPost) {
        return;
    }

    if (SqlRuntimeIsBusy()) {
        RedirectResponse(sock, "configure.html");
        return;
    }

    SqlNvClear();
    SqlRuntimeReloadFromNv();
    SqlConnectionStatusReset();
    SqlCatalogInvalidateTables();
    SqlDatabaseCatalogReset();

    RedirectResponse(sock, "configure.html");
}

HtmlPostVariableListCallback g_clear_nv_post("clearnv*", ClearNvPostCallback);
