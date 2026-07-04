// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>

#ifndef TDSLITE_SQL_NV_H
#define TDSLITE_SQL_NV_H

#include "sql_runtime.h"

void SqlNvInit();
bool SqlNvHasSaved();
bool SqlNvLoad(sql_runtime_config & cfg);
bool SqlNvSave(const sql_runtime_config & cfg);
bool SqlNvClear();
tdsl::uint32_t SqlNvLastVerifiedSecs();
void SqlNvSetLastVerifiedSecs(tdsl::uint32_t secs);

#endif
