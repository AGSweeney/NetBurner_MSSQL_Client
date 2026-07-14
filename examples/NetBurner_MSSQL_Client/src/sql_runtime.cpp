// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>

#include "sql_runtime.h"

#include <new>
#include <stdio.h>
#include <string.h>

#include <TDSLite/tdslite.h>

#include "sql_catalog.h"
#include "sql_nv.h"
#include "sql_results.h"

#ifdef NB_GATEWAY_MICRO800
#include "gateway/gateway_runtime.h"
#endif

// Quiet by default for routine browse/query chatter. Failures always log.
#ifndef SQL_RUNTIME_SERIAL_LOG
#define SQL_RUNTIME_SERIAL_LOG 0
#endif
#if SQL_RUNTIME_SERIAL_LOG
#define SQL_RT_LOG(...) iprintf(__VA_ARGS__)
#else
#define SQL_RT_LOG(...) ((void)0)
#endif
#define SQL_RT_FAIL(...) iprintf(__VA_ARGS__)

static sql_runtime_config g_config = {};
static volatile bool g_busy          = false;

enum sql_browse_job_kind : tdsl::uint8_t {
    SQL_BROWSE_NONE = 0,
    SQL_BROWSE_TABLES,
    SQL_BROWSE_DATABASES,
    SQL_BROWSE_COLUMNS,
    SQL_TEST_CONNECTION,
    SQL_EXECUTE_MUTATION,
};

static volatile sql_browse_job_kind g_browse_job = SQL_BROWSE_NONE;
static sql_connection_status g_connection_status = {};
static bool g_connection_verified                = false;
static tdsl::uint32_t g_last_verified_secs       = 0;
static sql_pending_mutation g_pending_mutation   = {};
static OS_SEM g_run_sem;
static OS_SEM g_request_lock;

static tdsl::uint8_t g_net_buf[4096] = {};

using sql_driver_t = tdsl::detail::tdsl_driver<tdsl::net::tdsl_netimpl_netburner>;

static sql_driver_t g_driver(g_net_buf);

static char g_query_buf[SQL_CFG_QUERY_LEN] = {};
static char g_last_sql_error[192]          = {};

// TDS sends UCS-2 little-endian. ColdFire is big-endian, so do not load as char16_t.
static void Utf16LeToAscii(const tdsl::span<const char16_t> &in, char *out, size_t outCap)
{
    if (!out || outCap == 0) {
        return;
    }
    const auto *bytes = reinterpret_cast<const unsigned char *>(in.data());
    const tdsl::size_t nbytes = in.size_bytes();
    size_t n = 0;
    for (tdsl::size_t i = 0; i + 1 < nbytes && n + 1 < outCap; i += 2) {
        const unsigned c = static_cast<unsigned>(bytes[i]) |
                           (static_cast<unsigned>(bytes[i + 1]) << 8);
        out[n++] = (c >= 32 && c < 127) ? static_cast<char>(c) : '?';
    }
    out[n] = '\0';
}

static void SqlInfoCallback(void * /*user*/, const tdsl::tds_info_token &info)
{
    if (!info.is_error()) {
        return;
    }
    char msg[160]{};
    Utf16LeToAscii(info.msgtext, msg, sizeof(msg));
    // 2628 = String or binary data would be truncated (value longer than column).
    if (info.number == 2628u && (msg[0] == '\0' || strspn(msg, "?") == strlen(msg))) {
        strncpy(msg, "String or binary data would be truncated (value longer than column)",
                sizeof(msg) - 1);
        msg[sizeof(msg) - 1] = '\0';
    }
    snprintf(g_last_sql_error, sizeof(g_last_sql_error), "SQL #%lu state=%u: %s",
             static_cast<unsigned long>(info.number), static_cast<unsigned>(info.state),
             msg[0] ? msg : "(no message)");
    SQL_RT_FAIL("SqlRuntime: %s\r\n", g_last_sql_error);
}

static void BindSqlInfoCallback()
{
    g_driver.set_info_callback(SqlInfoCallback, nullptr);
}

static void ClearLastSqlError()
{
    g_last_sql_error[0] = '\0';
}

// Clears busy + job together. Must not clear g_busy while g_browse_job still
// describes in-flight work — HTTP can race and sticky-busy with job NONE.
struct sql_run_scope_guard {
    ~sql_run_scope_guard()
    {
        g_browse_job = SQL_BROWSE_NONE;
        g_busy       = false;
    }
};

static tdsl::string_view SqlCStrView(const char * text) noexcept
{
    if (text == nullptr) {
        return {};
    }

    tdsl::size_t len = 0;
    while (text[len] != '\0') {
        ++len;
    }

    return tdsl::string_view(tdsl::char_view{text, len});
}

static void ResetSqlDriver()
{
    g_driver.disconnect();
    g_driver.~sql_driver_t();
    memset(g_net_buf, 0, sizeof(g_net_buf));
    new (static_cast<void*>(&g_driver)) sql_driver_t(g_net_buf);
    BindSqlInfoCallback();
}

static bool IsSafeSqlIdentifierChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_';
}

static bool IsSafeColumnsText(const char * text)
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }

    if (strcmp(text, "*") == 0) {
        return true;
    }

    for (const char * p = text; *p != '\0'; ++p) {
        const char c = *p;
        if (IsSafeSqlIdentifierChar(c) || c == ',' || c == ' ' || c == '[' || c == ']') {
            continue;
        }
        return false;
    }

    return true;
}

static bool IsSafeSqlIdentifier(const char * text)
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }

    for (const char * p = text; *p != '\0'; ++p) {
        if (!IsSafeSqlIdentifierChar(*p)) {
            return false;
        }
    }

    return true;
}

static bool WriteSqlStringLiteral(char * dst, tdsl::size_t cap, const char * value)
{
    if (cap < 3 || dst == nullptr || value == nullptr) {
        return false;
    }

    tdsl::size_t out = 0;
    dst[out++]       = '\'';
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
    dst[out]   = '\0';
    return true;
}

static bool BuildWhereClause(char * dst, tdsl::size_t cap, const sql_runtime_config & cfg)
{
    if (cap == 0) {
        return false;
    }
    dst[0] = '\0';

    if (cfg.filter_column[0] == '\0' || cfg.filter_op[0] == '\0') {
        return true;
    }

    if (!IsSafeSqlIdentifier(cfg.filter_column)) {
        return false;
    }

    if (strcmp(cfg.filter_op, "isnull") == 0) {
        return snprintf(dst, cap, " WHERE [%s] IS NULL", cfg.filter_column) > 0;
    }

    if (strcmp(cfg.filter_op, "isnotnull") == 0) {
        return snprintf(dst, cap, " WHERE [%s] IS NOT NULL", cfg.filter_column) > 0;
    }

    if (cfg.filter_value[0] == '\0') {
        return true;
    }

    char literal[SQL_CFG_FILTER_LEN * 2] = {};
    char like_value[SQL_CFG_FILTER_LEN + 4] = {};
    const char * op = nullptr;

    if (strcmp(cfg.filter_op, "eq") == 0) {
        op = "=";
        if (!WriteSqlStringLiteral(literal, sizeof(literal), cfg.filter_value)) {
            return false;
        }
    }
    else if (strcmp(cfg.filter_op, "ne") == 0) {
        op = "<>";
        if (!WriteSqlStringLiteral(literal, sizeof(literal), cfg.filter_value)) {
            return false;
        }
    }
    else if (strcmp(cfg.filter_op, "gt") == 0) {
        op = ">";
        if (!WriteSqlStringLiteral(literal, sizeof(literal), cfg.filter_value)) {
            return false;
        }
    }
    else if (strcmp(cfg.filter_op, "ge") == 0) {
        op = ">=";
        if (!WriteSqlStringLiteral(literal, sizeof(literal), cfg.filter_value)) {
            return false;
        }
    }
    else if (strcmp(cfg.filter_op, "lt") == 0) {
        op = "<";
        if (!WriteSqlStringLiteral(literal, sizeof(literal), cfg.filter_value)) {
            return false;
        }
    }
    else if (strcmp(cfg.filter_op, "le") == 0) {
        op = "<=";
        if (!WriteSqlStringLiteral(literal, sizeof(literal), cfg.filter_value)) {
            return false;
        }
    }
    else if (strcmp(cfg.filter_op, "contains") == 0) {
        op = "LIKE";
        snprintf(like_value, sizeof(like_value), "%%%s%%", cfg.filter_value);
        if (!WriteSqlStringLiteral(literal, sizeof(literal), like_value)) {
            return false;
        }
    }
    else if (strcmp(cfg.filter_op, "starts") == 0) {
        op = "LIKE";
        snprintf(like_value, sizeof(like_value), "%s%%", cfg.filter_value);
        if (!WriteSqlStringLiteral(literal, sizeof(literal), like_value)) {
            return false;
        }
    }
    else if (strcmp(cfg.filter_op, "ends") == 0) {
        op = "LIKE";
        snprintf(like_value, sizeof(like_value), "%%%s", cfg.filter_value);
        if (!WriteSqlStringLiteral(literal, sizeof(literal), like_value)) {
            return false;
        }
    }
    else {
        return false;
    }

    const int written =
        snprintf(dst, cap, " WHERE [%s] %s %s", cfg.filter_column, op, literal);
    return written > 0 && static_cast<tdsl::size_t>(written) < cap;
}

static bool BuildOrderByClause(char * dst, tdsl::size_t cap, const sql_runtime_config & cfg)
{
    if (cap == 0) {
        return false;
    }
    dst[0] = '\0';

    if (cfg.sort_column[0] == '\0') {
        return true;
    }

    if (!IsSafeSqlIdentifier(cfg.sort_column)) {
        return false;
    }

    const int written = snprintf(dst, cap, " ORDER BY [%s] %s", cfg.sort_column,
                                 cfg.sort_desc ? "DESC" : "ASC");
    return written > 0 && static_cast<tdsl::size_t>(written) < cap;
}

static bool BuildTableQuery(char * dst, tdsl::size_t cap, const sql_runtime_config & cfg)
{
    if (cap == 0 || cfg.database[0] == '\0' || cfg.table[0] == '\0') {
        return false;
    }

    if (!IsSafeSqlIdentifier(cfg.table)) {
        return false;
    }

    const char * columns = cfg.columns;
    if (columns[0] == '\0') {
        columns = "*";
    }

    if (!IsSafeColumnsText(columns)) {
        return false;
    }

    if (cfg.top_n == 0 || cfg.top_n > SQL_MAX_TOP_N) {
        return false;
    }

    char where_clause[256] = {};
    char order_clause[192] = {};
    if (!BuildWhereClause(where_clause, sizeof(where_clause), cfg) ||
        !BuildOrderByClause(order_clause, sizeof(order_clause), cfg)) {
        return false;
    }

    const int written =
        snprintf(dst, cap,
                 "SELECT TOP (%lu) %s FROM [%s].[dbo].[%s]%s%s",
                 static_cast<unsigned long>(cfg.top_n), columns, cfg.database, cfg.table,
                 where_clause, order_clause);

    return written > 0 && static_cast<tdsl::size_t>(written) < cap;
}

bool SqlRuntimeBuildTableQuery(char * dst, tdsl::size_t cap, const sql_runtime_config & cfg)
{
    return BuildTableQuery(dst, cap, cfg);
}

static bool SqlConnect(const sql_runtime_config & cfg,
                       tdsl::netburner_driver::e_driver_error_code & err_out,
                       const char * db_override = nullptr)
{
    tdsl::netburner_driver::connection_parameters params;
    params.server_name         = SqlCStrView(cfg.server);
    params.port                = cfg.port;
    params.user_name           = SqlCStrView(cfg.user);
    params.password            = SqlCStrView(cfg.password);
    params.client_name         = "NANO54415";
    params.app_name            = "NetBurner_MSSQL_Client";
    params.db_name             = SqlCStrView(db_override != nullptr ? db_override : cfg.database);
    params.packet_size         = 4096;
    params.conn_retry_count    = 3;
    params.conn_retry_delay_ms = 3000;

    const auto connect_result = g_driver.connect(params);
    err_out                   = connect_result;
    return connect_result == tdsl::netburner_driver::e_driver_error_code::success;
}

static void SqlBrowseTablesOnce()
{
    ResetSqlDriver();

    if (!tdsl::endian_self_test()) {
        sql_table_catalog & catalog = SqlCatalogGet();
        catalog.ready               = true;
        catalog.success             = false;
        strncpy(catalog.status, "Endian self-test failed.", sizeof(catalog.status) - 1);
        return;
    }

    sql_table_catalog & catalog = SqlCatalogGet();
    SqlCatalogReset();

    const sql_runtime_config cfg = g_config;
    strncpy(catalog.database, cfg.database, sizeof(catalog.database) - 1);

    if (cfg.server[0] == '\0' || cfg.user[0] == '\0' || cfg.database[0] == '\0') {
        catalog.ready   = true;
        catalog.success = false;
        strncpy(catalog.status, "Server, user, and database are required.", sizeof(catalog.status) - 1);
        return;
    }

    SQL_RT_LOG("SqlRuntime: browsing tables in %s on %s\r\n", cfg.database, cfg.server);

    tdsl::netburner_driver::e_driver_error_code connect_err =
        tdsl::netburner_driver::e_driver_error_code::connection_failed;
    if (!SqlConnect(cfg, connect_err)) {
        catalog.ready   = true;
        catalog.success = false;
        snprintf(catalog.status, sizeof(catalog.status), "SQL login failed (%d)",
                 static_cast<int>(connect_err));
        return;
    }

    g_driver.option_set_read_column_names(true);

    const auto query_result = g_driver.execute_query(SqlCStrView(SQL_BROWSE_TABLES_QUERY),
                                                     SqlCatalogRowCallback(), nullptr);

    g_driver.disconnect();

    catalog.ready   = true;
    catalog.success = static_cast<bool>(query_result);
    if (!query_result) {
        strncpy(catalog.status, "Failed to list tables.", sizeof(catalog.status) - 1);
        return;
    }

    if (catalog.table_count >= SQL_CATALOG_MAX_TABLES) {
        snprintf(catalog.status, sizeof(catalog.status),
                 "%lu+ tables (list truncated at %u).",
                 static_cast<unsigned long>(catalog.table_count), SQL_CATALOG_MAX_TABLES);
    }
    else {
        snprintf(catalog.status, sizeof(catalog.status), "%lu table(s) in %s.",
                 static_cast<unsigned long>(catalog.table_count), cfg.database);
    }

    SQL_RT_LOG("SqlRuntime: browse complete, %lu table(s)\r\n",
            static_cast<unsigned long>(catalog.table_count));
}

static bool BuildColumnsQuery(char * dst, tdsl::size_t cap, const sql_runtime_config & cfg)
{
    if (cap == 0 || cfg.table[0] == '\0') {
        return false;
    }

    for (const char * p = cfg.table; *p != '\0'; ++p) {
        if (!IsSafeSqlIdentifierChar(*p)) {
            return false;
        }
    }

    const int written =
        snprintf(dst, cap,
                 "SELECT COLUMN_NAME, DATA_TYPE FROM INFORMATION_SCHEMA.COLUMNS "
                 "WHERE TABLE_SCHEMA='dbo' AND TABLE_NAME='%s' ORDER BY ORDINAL_POSITION",
                 cfg.table);

    return written > 0 && static_cast<tdsl::size_t>(written) < cap;
}

static void SqlBrowseColumnsOnce()
{
    ResetSqlDriver();

    if (!tdsl::endian_self_test()) {
        sql_column_catalog & catalog = SqlColumnCatalogGet();
        catalog.ready                = true;
        catalog.success              = false;
        strncpy(catalog.status, "Endian self-test failed.", sizeof(catalog.status) - 1);
        return;
    }

    sql_column_catalog & catalog = SqlColumnCatalogGet();
    SqlColumnCatalogReset();

    const sql_runtime_config cfg = g_config;
    strncpy(catalog.database, cfg.database, sizeof(catalog.database) - 1);
    strncpy(catalog.table, cfg.table, sizeof(catalog.table) - 1);

    if (cfg.server[0] == '\0' || cfg.user[0] == '\0' || cfg.database[0] == '\0' ||
        cfg.table[0] == '\0') {
        catalog.ready   = true;
        catalog.success = false;
        strncpy(catalog.status, "Server, user, database, and table are required.",
                sizeof(catalog.status) - 1);
        return;
    }

    if (!BuildColumnsQuery(g_query_buf, sizeof(g_query_buf), cfg)) {
        catalog.ready   = true;
        catalog.success = false;
        strncpy(catalog.status, "Invalid table name.", sizeof(catalog.status) - 1);
        return;
    }

    SQL_RT_LOG("SqlRuntime: browsing columns in %s.%s\r\n", cfg.database, cfg.table);

    tdsl::netburner_driver::e_driver_error_code connect_err =
        tdsl::netburner_driver::e_driver_error_code::connection_failed;
    if (!SqlConnect(cfg, connect_err)) {
        catalog.ready   = true;
        catalog.success = false;
        snprintf(catalog.status, sizeof(catalog.status), "SQL login failed (%d)",
                 static_cast<int>(connect_err));
        return;
    }

    g_driver.option_set_read_column_names(true);

    const auto query_result = g_driver.execute_query(SqlCStrView(g_query_buf),
                                                     SqlColumnCatalogRowCallback(), nullptr);

    g_driver.disconnect();

    catalog.ready   = true;
    catalog.success = static_cast<bool>(query_result);
    if (!query_result) {
        strncpy(catalog.status, "Failed to list columns.", sizeof(catalog.status) - 1);
        return;
    }

    if (catalog.column_count >= SQL_CATALOG_MAX_COLUMNS) {
        snprintf(catalog.status, sizeof(catalog.status),
                 "%lu+ columns (list truncated at %u).",
                 static_cast<unsigned long>(catalog.column_count), SQL_CATALOG_MAX_COLUMNS);
    }
    else {
        snprintf(catalog.status, sizeof(catalog.status), "%lu column(s) in %s.",
                 static_cast<unsigned long>(catalog.column_count), cfg.table);
    }

    SQL_RT_LOG("SqlRuntime: column browse complete, %lu column(s)\r\n",
            static_cast<unsigned long>(catalog.column_count));
}

static void SqlTestConnectionOnce()
{
    ResetSqlDriver();

    g_connection_status.ready   = false;
    g_connection_status.success = false;
    g_connection_status.message[0] = '\0';

    if (!tdsl::endian_self_test()) {
        g_connection_status.ready = true;
        strncpy(g_connection_status.message, "Endian self-test failed.",
                sizeof(g_connection_status.message) - 1);
        return;
    }

    const sql_runtime_config cfg = g_config;

    if (cfg.server[0] == '\0' || cfg.user[0] == '\0' || cfg.database[0] == '\0' ||
        cfg.port == 0) {
        g_connection_status.ready = true;
        strncpy(g_connection_status.message,
                "Server, port, user, and database are required.",
                sizeof(g_connection_status.message) - 1);
        return;
    }

    SQL_RT_LOG("SqlRuntime: testing connection to %s:%u db=%s\r\n", cfg.server,
            static_cast<unsigned>(cfg.port), cfg.database);

    tdsl::netburner_driver::e_driver_error_code connect_err =
        tdsl::netburner_driver::e_driver_error_code::connection_failed;
    if (!SqlConnect(cfg, connect_err)) {
        g_connection_verified     = false;
        g_connection_status.ready = true;
        snprintf(g_connection_status.message, sizeof(g_connection_status.message),
                 "Connection failed (%d).", static_cast<int>(connect_err));
        SQL_RT_FAIL("SqlRuntime: connection test failed (%d)\r\n", static_cast<int>(connect_err));
        return;
    }

    g_driver.disconnect();

    g_connection_verified = true;
    g_last_verified_secs  = Secs;

    g_connection_status.ready   = true;
    g_connection_status.success = true;
    snprintf(g_connection_status.message, sizeof(g_connection_status.message),
             "Connected to %s. Settings are ready to save.", cfg.server);
    SQL_RT_LOG("SqlRuntime: connection test succeeded\r\n");
}

static void SqlBrowseDatabasesOnce()
{
    ResetSqlDriver();

    if (!tdsl::endian_self_test()) {
        sql_database_catalog & catalog = SqlDatabaseCatalogGet();
        catalog.ready                  = true;
        catalog.success                = false;
        strncpy(catalog.status, "Endian self-test failed.", sizeof(catalog.status) - 1);
        return;
    }

    sql_database_catalog & catalog = SqlDatabaseCatalogGet();
    SqlDatabaseCatalogReset();

    const sql_runtime_config cfg = g_config;

    if (cfg.server[0] == '\0' || cfg.user[0] == '\0' || cfg.port == 0) {
        catalog.ready   = true;
        catalog.success = false;
        strncpy(catalog.status, "Server, port, and user are required.", sizeof(catalog.status) - 1);
        return;
    }

    SQL_RT_LOG("SqlRuntime: browsing databases on %s\r\n", cfg.server);

    tdsl::netburner_driver::e_driver_error_code connect_err =
        tdsl::netburner_driver::e_driver_error_code::connection_failed;
    if (!SqlConnect(cfg, connect_err, "master")) {
        catalog.ready   = true;
        catalog.success = false;
        snprintf(catalog.status, sizeof(catalog.status), "SQL login failed (%d)",
                 static_cast<int>(connect_err));
        return;
    }

    g_driver.option_set_read_column_names(true);

    const auto query_result = g_driver.execute_query(SqlCStrView(SQL_BROWSE_DATABASES_QUERY),
                                                     SqlDatabaseCatalogRowCallback(), nullptr);

    g_driver.disconnect();

    catalog.ready   = true;
    catalog.success = static_cast<bool>(query_result);
    if (!query_result) {
        strncpy(catalog.status, "Failed to list databases.", sizeof(catalog.status) - 1);
        return;
    }

    if (catalog.database_count >= SQL_CATALOG_MAX_DATABASES) {
        snprintf(catalog.status, sizeof(catalog.status),
                 "%lu+ databases (list truncated at %u).",
                 static_cast<unsigned long>(catalog.database_count), SQL_CATALOG_MAX_DATABASES);
    }
    else {
        snprintf(catalog.status, sizeof(catalog.status), "%lu database(s) on server.",
                 static_cast<unsigned long>(catalog.database_count));
    }

    SQL_RT_LOG("SqlRuntime: database browse complete, %lu database(s)\r\n",
            static_cast<unsigned long>(catalog.database_count));
}

static void SqlExecuteMutationOnce()
{
    ResetSqlDriver();

    sql_result_store & store = SqlResultsGet();
    SqlResultsReset();
    store.is_mutation = true;

    if (!g_pending_mutation.valid || g_pending_mutation.sql[0] == '\0') {
        store.ready   = true;
        store.success = false;
        strncpy(store.status, "No pending write operation to execute.", sizeof(store.status) - 1);
        return;
    }

    const sql_runtime_config cfg = g_config;
    strncpy(store.database, cfg.database, sizeof(store.database) - 1);
    strncpy(store.table_label, "Write", sizeof(store.table_label) - 1);
    strncpy(store.page_heading, "Write operation", sizeof(store.page_heading) - 1);

    if (cfg.server[0] == '\0' || cfg.user[0] == '\0' || cfg.database[0] == '\0' ||
        cfg.port == 0) {
        store.ready   = true;
        store.success = false;
        strncpy(store.status, "Server, port, user, and database are required.",
                sizeof(store.status) - 1);
        return;
    }

    ClearLastSqlError();
    SQL_RT_LOG("SqlRuntime: executing confirmed write\r\n");
    SQL_RT_LOG("SqlRuntime: %s\r\n", g_pending_mutation.sql);

    tdsl::netburner_driver::e_driver_error_code connect_err =
        tdsl::netburner_driver::e_driver_error_code::connection_failed;
    if (!SqlConnect(cfg, connect_err)) {
        store.ready   = true;
        store.success = false;
        snprintf(store.status, sizeof(store.status), "SQL login failed (%d)",
                 static_cast<int>(connect_err));
        SQL_RT_FAIL("SqlRuntime: write login failed (%d) sql=%s\r\n",
                    static_cast<int>(connect_err), g_pending_mutation.sql);
        return;
    }

    const unsigned long query_start_ticks = TimeTick;
    const auto query_result =
        g_driver.execute_query(SqlCStrView(g_pending_mutation.sql), SqlResultsRowCallback(), nullptr);
    const unsigned long query_elapsed_ticks = TimeTick - query_start_ticks;
    g_driver.disconnect();

    store.ready             = true;
    store.success           = static_cast<bool>(query_result);
    store.query_duration_ms =
        static_cast<tdsl::uint32_t>((query_elapsed_ticks * 1000ul) / TICKS_PER_SECOND);

    if (store.success) {
        snprintf(store.status, sizeof(store.status), "Write completed: %s",
                 g_pending_mutation.summary);
        SQL_RT_LOG("SqlRuntime: write complete\r\n");
#ifdef NB_GATEWAY_MICRO800
        {
            const uint64_t eid = gateway::TakeInflightEventId();
            if (eid != 0) {
                gateway::NotifySqlCommitted(eid);
            }
        }
#endif
        SqlRuntimeClearPendingMutation();
    }
    else {
        if (g_last_sql_error[0]) {
            strncpy(store.status, g_last_sql_error, sizeof(store.status) - 1);
            store.status[sizeof(store.status) - 1] = '\0';
            SQL_RT_FAIL("SqlRuntime: write failed: %s\r\n  sql: %s\r\n", g_last_sql_error,
                        g_pending_mutation.sql);
        } else {
            snprintf(store.status, sizeof(store.status),
                     "Write failed (TDS status=0x%X, rows=%lu)",
                     static_cast<unsigned>(query_result.status.value),
                     static_cast<unsigned long>(query_result.affected_rows));
            SQL_RT_FAIL("SqlRuntime: write failed (no INFO token): status=0x%X rows=%lu sql=%s\r\n",
                        static_cast<unsigned>(query_result.status.value),
                        static_cast<unsigned long>(query_result.affected_rows),
                        g_pending_mutation.sql);
        }
#ifdef NB_GATEWAY_MICRO800
        // Leave event pending for retry with backoff (avoids busy-spin QUERY RUNNING loop).
        gateway::NotifySqlWriteFailed();
#endif
    }
}

static void SqlRunOnce()
{
    // Keep g_browse_job set for the duration of the work so the UI can distinguish
    // catalog browse / connection test from a real query (IsRunningQuery).
    // Clear busy+job together at scope end so HTTP cannot observe busy with job NONE.
    sql_run_scope_guard scope_guard;

    if (g_browse_job == SQL_BROWSE_TABLES) {
        SqlBrowseTablesOnce();
        return;
    }

    if (g_browse_job == SQL_BROWSE_DATABASES) {
        SqlBrowseDatabasesOnce();
        return;
    }

    if (g_browse_job == SQL_BROWSE_COLUMNS) {
        SqlBrowseColumnsOnce();
        return;
    }

    if (g_browse_job == SQL_TEST_CONNECTION) {
        SqlTestConnectionOnce();
        return;
    }

    if (g_browse_job == SQL_EXECUTE_MUTATION) {
        SqlExecuteMutationOnce();
        return;
    }

    ResetSqlDriver();

    if (!tdsl::endian_self_test()) {
        sql_result_store & store = SqlResultsGet();
        store.ready              = true;
        store.success            = false;
        strncpy(store.status, "Endian self-test failed.", sizeof(store.status) - 1);
        SQL_RT_FAIL("SqlRuntime: endian self-test failed\r\n");
        return;
    }

    sql_result_store & store = SqlResultsGet();
    SqlResultsReset();

    const sql_runtime_config cfg = g_config;

    strncpy(store.database, cfg.database, sizeof(store.database) - 1);
    if (cfg.use_table_builder && cfg.table[0] != '\0') {
        strncpy(store.table_label, cfg.table, sizeof(store.table_label) - 1);
        snprintf(store.page_heading, sizeof(store.page_heading), "%s %s", cfg.database, cfg.table);
    }
    else {
        strncpy(store.table_label, "Query", sizeof(store.table_label) - 1);
        snprintf(store.page_heading, sizeof(store.page_heading), "%s query", cfg.database);
    }

    const char * query_text = cfg.query;

    if (cfg.use_table_builder) {
        if (!BuildTableQuery(g_query_buf, sizeof(g_query_buf), cfg)) {
            store.ready   = true;
            store.success = false;
            strncpy(store.status, "Invalid table read settings.", sizeof(store.status) - 1);
            SQL_RT_FAIL("SqlRuntime: invalid table builder settings\r\n");
            return;
        }
        query_text = g_query_buf;
    }
    else if (query_text[0] == '\0') {
        store.ready   = true;
        store.success = false;
        strncpy(store.status, "SQL query is empty.", sizeof(store.status) - 1);
        SQL_RT_FAIL("SqlRuntime: empty query\r\n");
        return;
    }

    if (cfg.server[0] == '\0' || cfg.user[0] == '\0' || cfg.database[0] == '\0' ||
        cfg.port == 0) {
        store.ready   = true;
        store.success = false;
        strncpy(store.status, "Server, port, user, and database are required.",
                sizeof(store.status) - 1);
        SQL_RT_FAIL("SqlRuntime: missing connection fields\r\n");
        return;
    }

    SQL_RT_LOG("SqlRuntime: connecting to %s:%u db=%s\r\n", cfg.server,
            static_cast<unsigned>(cfg.port), cfg.database);

    tdsl::netburner_driver::e_driver_error_code connect_err =
        tdsl::netburner_driver::e_driver_error_code::connection_failed;
    if (!SqlConnect(cfg, connect_err)) {
        store.ready   = true;
        store.success = false;
        snprintf(store.status, sizeof(store.status), "SQL login failed (%d)",
                 static_cast<int>(connect_err));
        SQL_RT_FAIL("SqlRuntime: connect/login failed (%d) server=%s db=%s\r\n",
                    static_cast<int>(connect_err), cfg.server, cfg.database);
        return;
    }

    g_driver.option_set_read_column_names(true);

    SQL_RT_LOG("SqlRuntime: running query\r\n");
    SQL_RT_LOG("SqlRuntime: %s\r\n", query_text);

    const unsigned long query_start_ticks = TimeTick;
    const auto query_result =
        g_driver.execute_query(SqlCStrView(query_text), SqlResultsRowCallback(), nullptr);
    const unsigned long query_elapsed_ticks = TimeTick - query_start_ticks;

    g_driver.disconnect();

    store.ready   = true;
    store.success = static_cast<bool>(query_result);
    store.query_duration_ms =
        static_cast<tdsl::uint32_t>((query_elapsed_ticks * 1000ul) / TICKS_PER_SECOND);
    snprintf(store.status, sizeof(store.status), "%s (%s)", cfg.server,
             tdsl::endian_is_little() ? "LE" : "BE");

    if (!query_result) {
        snprintf(store.status, sizeof(store.status), "Query failed on %s.", cfg.server);
        SQL_RT_FAIL("SqlRuntime: query failed: %s\r\n", query_text);
        return;
    }

    SQL_RT_LOG("SqlRuntime: query complete, %lu row(s)\r\n",
            static_cast<unsigned long>(store.row_count));
}

void SqlRuntimeApplyQueryDefaults(sql_runtime_config & cfg)
{
    if (cfg.columns[0] == '\0') {
        strncpy(cfg.columns, SQL_DEFAULT_COLUMNS, sizeof(cfg.columns) - 1);
    }

    if (cfg.top_n == 0 || cfg.top_n > SQL_MAX_TOP_N) {
        cfg.top_n = SQL_DEFAULT_TOP_N;
    }
}

void SqlRuntimeInitDefaults()
{
    memset(&g_config, 0, sizeof(g_config));

    SqlNvInit();

    sql_runtime_config nv_cfg = {};
    if (SqlNvLoad(nv_cfg)) {
        g_config = nv_cfg;
    }
    else {
        g_config.port = SQL_DEFAULT_PORT;
        SqlRuntimeApplyQueryDefaults(g_config);
    }

    g_connection_verified = SqlNvHasSaved();
    g_last_verified_secs  = SqlNvLastVerifiedSecs();
}

bool SqlRuntimeIsConnectionVerified()
{
    return g_connection_verified;
}

tdsl::uint32_t SqlRuntimeLastVerifiedSecs()
{
    return g_last_verified_secs;
}

void SqlRuntimeMarkConnectionVerified()
{
    g_connection_verified  = true;
    g_last_verified_secs   = Secs;
    SqlNvSetLastVerifiedSecs(Secs);
}

bool SqlRuntimeSaveCurrentConfig()
{
    if (!g_connection_verified) {
        g_connection_status.ready   = true;
        g_connection_status.success = false;
        strncpy(g_connection_status.message,
                "Test the connection successfully before saving settings.",
                sizeof(g_connection_status.message) - 1);
        return false;
    }

    SqlNvSetLastVerifiedSecs(g_last_verified_secs);
    const bool saved = SqlNvSave(g_config);

    g_connection_status.ready   = true;
    g_connection_status.success = saved;
    if (saved) {
        strncpy(g_connection_status.message, "Settings saved to NV flash.",
                sizeof(g_connection_status.message) - 1);
        SQL_RT_LOG("SqlRuntime: settings saved to NV flash\r\n");
    }
    else {
        strncpy(g_connection_status.message, "Saved-settings error.",
                sizeof(g_connection_status.message) - 1);
        SQL_RT_FAIL("SqlRuntime: failed to save settings\r\n");
    }

    return saved;
}

void SqlRuntimeClearConnectionVerified()
{
    g_connection_verified = false;
    g_last_verified_secs  = 0;
}

void SqlRuntimeReloadFromNv()
{
    SqlRuntimeInitDefaults();
}

void SqlConnectionStatusReset()
{
    memset(&g_connection_status, 0, sizeof(g_connection_status));
    SqlRuntimeClearConnectionVerified();
}

const sql_runtime_config & SqlRuntimeGetConfig()
{
    return g_config;
}

bool SqlRuntimeIsBusy()
{
    return g_busy;
}

bool SqlRuntimeIsRunningQuery()
{
    // Browse/test/mutations keep their own job flags; only manual SELECT/query
    // counts as "query running" in the header/banner.
    return g_busy && g_browse_job == SQL_BROWSE_NONE;
}

bool SqlRuntimeIsExecutingMutation()
{
    return g_busy && g_browse_job == SQL_EXECUTE_MUTATION;
}

bool SqlRuntimeIsTestingConnection()
{
    return g_busy && g_browse_job == SQL_TEST_CONNECTION;
}

bool SqlRuntimeIsBrowsingTables()
{
    return g_busy && g_browse_job == SQL_BROWSE_TABLES;
}

bool SqlRuntimeIsBrowsingDatabases()
{
    return g_busy && g_browse_job == SQL_BROWSE_DATABASES;
}

bool SqlRuntimeIsBrowsingColumns()
{
    return g_busy && g_browse_job == SQL_BROWSE_COLUMNS;
}

const sql_connection_status & SqlConnectionStatusGet()
{
    return g_connection_status;
}

void SqlRuntimeInit()
{
    OSSemInit(&g_run_sem, 0);
    OSSemInit(&g_request_lock, 1);
}

void SqlRuntimePoll()
{
    if (OSSemPendNoWait(&g_run_sem) != OS_NO_ERR) {
        return;
    }

    while (OSSemPendNoWait(&g_run_sem) == OS_NO_ERR) {
    }

    SqlRunOnce();
}

void SqlRuntimeRequestRun()
{
    OSSemPend(&g_request_lock, 0);

    if (g_busy) {
        OSSemPost(&g_request_lock);
        return;
    }

    g_browse_job = SQL_BROWSE_NONE;
    g_busy       = true;
    OSSemPost(&g_request_lock);
    OSSemPost(&g_run_sem);
}

void SqlRuntimeRequestTestConnection()
{
    OSSemPend(&g_request_lock, 0);

    if (g_busy) {
        OSSemPost(&g_request_lock);
        return;
    }

    g_browse_job = SQL_TEST_CONNECTION;
    g_busy       = true;
    OSSemPost(&g_request_lock);
    OSSemPost(&g_run_sem);
}

void SqlRuntimeRequestBrowseTables()
{
    OSSemPend(&g_request_lock, 0);

    if (g_busy) {
        OSSemPost(&g_request_lock);
        return;
    }

    g_browse_job = SQL_BROWSE_TABLES;
    g_busy       = true;
    OSSemPost(&g_request_lock);
    OSSemPost(&g_run_sem);
}

void SqlRuntimeRequestBrowseDatabases()
{
    OSSemPend(&g_request_lock, 0);

    if (g_busy) {
        OSSemPost(&g_request_lock);
        return;
    }

    g_browse_job = SQL_BROWSE_DATABASES;
    g_busy       = true;
    OSSemPost(&g_request_lock);
    OSSemPost(&g_run_sem);
}

void SqlRuntimeRequestBrowseColumns()
{
    OSSemPend(&g_request_lock, 0);

    if (g_busy) {
        OSSemPost(&g_request_lock);
        return;
    }

    g_browse_job = SQL_BROWSE_COLUMNS;
    g_busy       = true;
    OSSemPost(&g_request_lock);
    OSSemPost(&g_run_sem);
}

void SqlRuntimeRequestExecutePendingMutation()
{
    OSSemPend(&g_request_lock, 0);

    if (g_busy) {
        OSSemPost(&g_request_lock);
        return;
    }

    g_browse_job = SQL_EXECUTE_MUTATION;
    g_busy       = true;
    OSSemPost(&g_request_lock);
    OSSemPost(&g_run_sem);
}

void SqlRuntimeUpdateConfig(const sql_runtime_config & cfg)
{
    // Invalidate catalogs only when the destination identity changes. Using
    // Matches() here also treated failed browses as "mismatch" and cleared them,
    // which made the maps page re-request forever on SQL errors.
    const sql_table_catalog &tables = SqlCatalogGet();
    if (cfg.database[0] != '\0' && tables.database[0] != '\0' &&
        strcmp(tables.database, cfg.database) != 0) {
        SqlCatalogInvalidateTables();
    }

    const sql_column_catalog &cols = SqlColumnCatalogGet();
    if (cfg.table[0] == '\0') {
        if (cols.database[0] != '\0' || cols.table[0] != '\0' || cols.ready) {
            SqlColumnCatalogInvalidate();
        }
    } else if (cfg.database[0] != '\0' &&
               (cols.database[0] != '\0' || cols.table[0] != '\0') &&
               (strcmp(cols.database, cfg.database) != 0 || strcmp(cols.table, cfg.table) != 0)) {
        SqlColumnCatalogInvalidate();
    }

    g_config = cfg;
    SqlRuntimeApplyQueryDefaults(g_config);
}

const sql_pending_mutation & SqlRuntimeGetPendingMutation()
{
    return g_pending_mutation;
}

bool SqlRuntimeStageMutation(sql_mutation_kind kind, const char * sql, const char * summary)
{
    if (sql == nullptr || sql[0] == '\0') {
        SqlRuntimeClearPendingMutation();
        return false;
    }

    memset(&g_pending_mutation, 0, sizeof(g_pending_mutation));
    g_pending_mutation.valid = true;
    g_pending_mutation.kind  = kind;
    strncpy(g_pending_mutation.sql, sql, sizeof(g_pending_mutation.sql) - 1);
    if (summary != nullptr) {
        strncpy(g_pending_mutation.summary, summary, sizeof(g_pending_mutation.summary) - 1);
    }
    return true;
}

void SqlRuntimeClearPendingMutation()
{
    memset(&g_pending_mutation, 0, sizeof(g_pending_mutation));
}
