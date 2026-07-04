// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>

#ifndef TDSLITE_SQL_RESULTS_H
#define TDSLITE_SQL_RESULTS_H

#include <TDSLite/tdslite.h>

#define SQL_RESULT_MAX_COLS  32
#define SQL_RESULT_MAX_ROWS  50
#define SQL_RESULT_COL_NAME  32
#define SQL_RESULT_CELL_LEN  128
#define SQL_RESULT_LABEL_LEN 48

struct sql_result_store {
    bool ready;
    bool success;
    bool is_mutation;
    char status[160];
    char database[SQL_RESULT_LABEL_LEN];
    char table_label[SQL_RESULT_LABEL_LEN];
    char page_heading[64];
    tdsl::uint32_t query_duration_ms;
    tdsl::uint32_t col_count;
    char col_names[SQL_RESULT_MAX_COLS][SQL_RESULT_COL_NAME];
    tdsl::uint32_t row_count;
    char cells[SQL_RESULT_MAX_ROWS][SQL_RESULT_MAX_COLS][SQL_RESULT_CELL_LEN];
};

void SqlResultsReset();
sql_result_store & SqlResultsGet();

tdsl::netburner_driver::sql_command_row_callback SqlResultsRowCallback();

#endif
