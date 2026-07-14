// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>

#include "sql_nv.h"

#include <stdio.h>
#include <string.h>

#include <config_obj.h>
#include <config_server.h>

// Serial console chatter for NV save/clear. Set to 1 to re-enable.
#ifndef SQL_RUNTIME_SERIAL_LOG
#define SQL_RUNTIME_SERIAL_LOG 0
#endif
#if SQL_RUNTIME_SERIAL_LOG
#define SQL_NV_LOG(...) iprintf(__VA_ARGS__)
#else
#define SQL_NV_LOG(...) ((void)0)
#endif

static config_obj g_sql_nv_root{appdata, "SqlSettings", "SQL connection settings"};

static config_bool g_sql_nv_valid{g_sql_nv_root, false, "Valid", "Saved SQL settings present"};

static config_string g_sql_nv_server{g_sql_nv_root, "", "Server", "SQL server host"};
static config_int g_sql_nv_port{g_sql_nv_root, 0, "Port", "SQL server port"};
static config_string g_sql_nv_user{g_sql_nv_root, "", "User", "SQL login user"};
static config_pass g_sql_nv_password{g_sql_nv_root, "", "Password", "SQL login password"};
static config_string g_sql_nv_database{g_sql_nv_root, "", "Database", "Default database"};
static config_string g_sql_nv_table{g_sql_nv_root, "", "Table", "Default table"};
static config_string g_sql_nv_columns{g_sql_nv_root, "", "Columns", "Table builder columns"};
static config_int g_sql_nv_top_n{g_sql_nv_root, 0, "TopN", "Table builder TOP rows"};
static config_string g_sql_nv_filter_column{g_sql_nv_root, "", "FilterColumn", "Builder filter column"};
static config_string g_sql_nv_filter_op{g_sql_nv_root, "", "FilterOp", "Builder filter operator"};
static config_string g_sql_nv_filter_value{g_sql_nv_root, "", "FilterValue", "Builder filter value"};
static config_string g_sql_nv_sort_column{g_sql_nv_root, "", "SortColumn", "Builder sort column"};
static config_bool g_sql_nv_sort_desc{g_sql_nv_root, false, "SortDesc", "Builder sort descending"};
static config_bool g_sql_nv_use_table_builder{g_sql_nv_root, false, "UseTableBuilder",
                                             "Use table builder mode"};
static config_string g_sql_nv_query{g_sql_nv_root, "", "Query", "Custom SQL query"};
static config_int g_sql_nv_last_verified{g_sql_nv_root, 0, "LastVerified",
                                         "Uptime seconds at last successful test"};

static bool g_sql_nv_flags_set = false;

static void CopyToBuffer(char * dst, tdsl::size_t cap, const char * src)
{
    if (cap == 0) {
        return;
    }

    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }

    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

static bool SqlNvHasStoredData()
{
    return g_sql_nv_valid || g_sql_nv_server.length() > 0;
}

void SqlNvInit()
{
    if (g_sql_nv_flags_set) {
        return;
    }

    g_sql_nv_root.ClrBranchFlag(fConfigHidden, false);
    g_sql_nv_flags_set = true;
}

bool SqlNvHasSaved()
{
    return SqlNvHasStoredData();
}

bool SqlNvLoad(sql_runtime_config & cfg)
{
    if (!SqlNvHasStoredData()) {
        return false;
    }

    memset(&cfg, 0, sizeof(cfg));

    CopyToBuffer(cfg.server, sizeof(cfg.server), g_sql_nv_server.c_str());
    cfg.port = static_cast<tdsl::uint16_t>(static_cast<int>(g_sql_nv_port));
    CopyToBuffer(cfg.user, sizeof(cfg.user), g_sql_nv_user.c_str());
    CopyToBuffer(cfg.password, sizeof(cfg.password), g_sql_nv_password.c_str());
    CopyToBuffer(cfg.database, sizeof(cfg.database), g_sql_nv_database.c_str());
    CopyToBuffer(cfg.table, sizeof(cfg.table), g_sql_nv_table.c_str());
    CopyToBuffer(cfg.columns, sizeof(cfg.columns), g_sql_nv_columns.c_str());
    cfg.top_n             = static_cast<tdsl::uint32_t>(static_cast<int>(g_sql_nv_top_n));
    CopyToBuffer(cfg.filter_column, sizeof(cfg.filter_column), g_sql_nv_filter_column.c_str());
    CopyToBuffer(cfg.filter_op, sizeof(cfg.filter_op), g_sql_nv_filter_op.c_str());
    CopyToBuffer(cfg.filter_value, sizeof(cfg.filter_value), g_sql_nv_filter_value.c_str());
    CopyToBuffer(cfg.sort_column, sizeof(cfg.sort_column), g_sql_nv_sort_column.c_str());
    cfg.sort_desc         = static_cast<bool>(g_sql_nv_sort_desc);
    cfg.use_table_builder = static_cast<bool>(g_sql_nv_use_table_builder);
    CopyToBuffer(cfg.query, sizeof(cfg.query), g_sql_nv_query.c_str());

    if (cfg.port == 0) {
        cfg.port = SQL_DEFAULT_PORT;
    }

    if (cfg.columns[0] == '\0') {
        strncpy(cfg.columns, SQL_DEFAULT_COLUMNS, sizeof(cfg.columns) - 1);
    }

    if (cfg.top_n == 0) {
        cfg.top_n = SQL_DEFAULT_TOP_N;
    }

    return true;
}

bool SqlNvSave(const sql_runtime_config & cfg)
{
    g_sql_nv_server            = cfg.server;
    g_sql_nv_port              = static_cast<int>(cfg.port);
    g_sql_nv_user              = cfg.user;
    g_sql_nv_password          = cfg.password;
    g_sql_nv_database          = cfg.database;
    g_sql_nv_table             = cfg.table;
    g_sql_nv_columns           = cfg.columns;
    g_sql_nv_top_n             = static_cast<int>(cfg.top_n);
    g_sql_nv_filter_column     = cfg.filter_column;
    g_sql_nv_filter_op         = cfg.filter_op;
    g_sql_nv_filter_value      = cfg.filter_value;
    g_sql_nv_sort_column       = cfg.sort_column;
    g_sql_nv_sort_desc         = cfg.sort_desc;
    g_sql_nv_use_table_builder = cfg.use_table_builder;
    g_sql_nv_query             = cfg.query;
    g_sql_nv_valid             = true;

    SaveConfigToStorage();
    SQL_NV_LOG("SqlNv: saved SQL settings to AppData/SqlSettings\r\n");
    return true;
}

tdsl::uint32_t SqlNvLastVerifiedSecs()
{
    const int secs = static_cast<int>(g_sql_nv_last_verified);
    return secs > 0 ? static_cast<tdsl::uint32_t>(secs) : 0;
}

void SqlNvSetLastVerifiedSecs(tdsl::uint32_t secs)
{
    g_sql_nv_last_verified = static_cast<int>(secs);
}

bool SqlNvClear()
{
    g_sql_nv_server            = "";
    g_sql_nv_port              = 0;
    g_sql_nv_user              = "";
    g_sql_nv_password          = "";
    g_sql_nv_database          = "";
    g_sql_nv_table             = "";
    g_sql_nv_columns           = "";
    g_sql_nv_top_n             = 0;
    g_sql_nv_filter_column     = "";
    g_sql_nv_filter_op         = "";
    g_sql_nv_filter_value      = "";
    g_sql_nv_sort_column       = "";
    g_sql_nv_sort_desc         = false;
    g_sql_nv_use_table_builder = false;
    g_sql_nv_query             = "";
    g_sql_nv_valid             = false;
    g_sql_nv_last_verified     = 0;

    SaveConfigToStorage();
    SQL_NV_LOG("SqlNv: cleared SQL settings from AppData/SqlSettings\r\n");
    return true;
}
