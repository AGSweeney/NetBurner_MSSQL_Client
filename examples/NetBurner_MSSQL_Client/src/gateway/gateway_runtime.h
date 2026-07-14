// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>

#ifndef GATEWAY_RUNTIME_H
#define GATEWAY_RUNTIME_H

#include "gateway_types.h"

namespace gateway {

void RuntimeInit();
void RuntimeShutdown();
// Rebuild mapping runtimes / active flag from ConfigGet() after ConfigSave.
void RuntimeReloadFromConfig();

// Fair multi-mapping poll / handshake step. Call from UserMain or gateway task.
void RuntimePoll();

bool RuntimeIsActive();
void RuntimeSetActive(bool active);
bool HasInflightEvent();

MappingRuntime *RuntimeFind(uint32_t mappingId);
const MappingRuntime *RuntimeFindConst(uint32_t mappingId);

// SQL commit hooks (sql_runtime calls NotifySqlCommitted after success).
void SetInflightEventId(uint64_t id);
uint64_t TakeInflightEventId();
void NotifySqlCommitted(uint64_t eventId);
// Clear inflight, apply drain backoff, quarantine after repeated failures.
void NotifySqlWriteFailed();

} // namespace gateway

#endif
