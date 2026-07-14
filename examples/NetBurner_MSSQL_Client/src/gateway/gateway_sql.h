// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>

#ifndef GATEWAY_SQL_H
#define GATEWAY_SQL_H

#include "gateway_types.h"

namespace gateway {

// Build INSERT/UPDATE text from a captured event. Returns false on validation failure.
bool BuildSqlFromEvent(const CapturedEvent &ev, char *sqlOut, size_t sqlCap, char *summaryOut, size_t summaryCap,
                       char *err, size_t errCap);

// Static preview using tag names as placeholders (for mapping builder UI).
bool BuildSqlPreviewFromMapping(const MappingConfig &map, char *sqlOut, size_t sqlCap, char *err, size_t errCap);

// Request SQL runtime to process pending gateway queue (non-blocking).
void SqlRequestProcessBatch();

// Called from sql_runtime when NB_GATEWAY_MICRO800 is defined.
bool SqlDrainOnePending(char *err, size_t errCap);

} // namespace gateway

#endif
