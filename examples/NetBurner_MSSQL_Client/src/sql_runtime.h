// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>

#ifndef TDSLITE_SQL_RUNTIME_H
#define TDSLITE_SQL_RUNTIME_H

#include <nbrtos.h>

#include <tdslite/util/tdsl_inttypes.hpp>

#define SQL_CFG_SERVER_LEN   64
#define SQL_CFG_USER_LEN     64
#define SQL_CFG_PASSWORD_LEN 64
#define SQL_CFG_DATABASE_LEN 64
#define SQL_CFG_TABLE_LEN    128
#define SQL_CFG_COLUMNS_LEN  256
#define SQL_CFG_FILTER_LEN   128
#define SQL_CFG_OP_LEN       16
#define SQL_CFG_QUERY_LEN    768
#define SQL_MUTATION_SUMMARY_LEN 160

#define SQL_DEFAULT_PORT     1433
#define SQL_DEFAULT_COLUMNS  "*"
#define SQL_DEFAULT_TOP_N    100
#define SQL_MAX_TOP_N        250

struct sql_runtime_config {
    char server[SQL_CFG_SERVER_LEN];
    tdsl::uint16_t port;
    char user[SQL_CFG_USER_LEN];
    char password[SQL_CFG_PASSWORD_LEN];
    char database[SQL_CFG_DATABASE_LEN];
    char table[SQL_CFG_TABLE_LEN];
    char columns[SQL_CFG_COLUMNS_LEN];
    tdsl::uint32_t top_n;
    char filter_column[SQL_CFG_TABLE_LEN];
    char filter_op[SQL_CFG_OP_LEN];
    char filter_value[SQL_CFG_FILTER_LEN];
    char sort_column[SQL_CFG_TABLE_LEN];
    bool sort_desc;
    bool use_table_builder;
    char query[SQL_CFG_QUERY_LEN];
};

struct sql_connection_status {
    bool ready;
    bool success;
    char message[128];
};

enum sql_mutation_kind : tdsl::uint8_t {
    SQL_MUTATION_NONE = 0,
    SQL_MUTATION_INSERT,
    SQL_MUTATION_UPDATE,
    SQL_MUTATION_CUSTOM,
};

struct sql_pending_mutation {
    bool valid;
    sql_mutation_kind kind;
    char sql[SQL_CFG_QUERY_LEN];
    char summary[SQL_MUTATION_SUMMARY_LEN];
};

bool SqlRuntimeIsConnectionVerified();
tdsl::uint32_t SqlRuntimeLastVerifiedSecs();
void SqlRuntimeMarkConnectionVerified();
void SqlRuntimeClearConnectionVerified();

void SqlRuntimeApplyQueryDefaults(sql_runtime_config & cfg);
void SqlRuntimeInitDefaults();
void SqlRuntimeReloadFromNv();
const sql_runtime_config & SqlRuntimeGetConfig();
bool SqlRuntimeIsBusy();

void SqlRuntimeInit();
void SqlRuntimePoll();
void SqlRuntimeRequestRun();
void SqlRuntimeRequestTestConnection();
void SqlRuntimeRequestBrowseTables();
void SqlRuntimeRequestBrowseDatabases();
void SqlRuntimeRequestBrowseColumns();
void SqlRuntimeRequestExecutePendingMutation();
bool SqlRuntimeIsRunningQuery();
bool SqlRuntimeIsExecutingMutation();
bool SqlRuntimeIsTestingConnection();
bool SqlRuntimeIsBrowsingTables();
bool SqlRuntimeIsBrowsingDatabases();
bool SqlRuntimeIsBrowsingColumns();
void SqlConnectionStatusReset();
const sql_connection_status & SqlConnectionStatusGet();
void SqlRuntimeUpdateConfig(const sql_runtime_config & cfg);
bool SqlRuntimeSaveCurrentConfig();
bool SqlRuntimeBuildTableQuery(char * dst, tdsl::size_t cap, const sql_runtime_config & cfg);
const sql_pending_mutation & SqlRuntimeGetPendingMutation();
bool SqlRuntimeStageMutation(sql_mutation_kind kind, const char * sql, const char * summary);
void SqlRuntimeClearPendingMutation();

#endif
