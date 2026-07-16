// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
//
// Event queue. Uses an in-RAM ring (survives only while powered). EFFS-STD
// segmented store code remains available but is not enabled by QueueInit. ACK must only be asserted after QueueAppend succeeds.

#ifndef GATEWAY_QUEUE_H
#define GATEWAY_QUEUE_H

#include "gateway_types.h"

namespace gateway {

enum class QueuePressure : uint8_t {
    Normal = 0,
    Warning,
    Critical,
    Full,
    Fault,
};

enum class QueueBackend : uint8_t {
    Ram = 0,
    Effs = 1,
};

struct QueueStats {
    uint32_t record_count{0};
    uint32_t bytes_used{0};
    uint32_t bytes_budget{kQueueBudgetBytesDefault};
    uint32_t pending{0};
    uint32_t pending_capacity{0};
    uint32_t committed{0};
    uint32_t quarantined{0};
    uint32_t dropped_or_full{0};
    uint64_t next_event_id{1};
    QueuePressure pressure{QueuePressure::Normal};
    QueueBackend backend{QueueBackend::Ram};
};

bool QueueInit();
void QueueShutdown();

// Persist complete event. Returns false if full/fault (do not ACK).
bool QueueAppend(const CapturedEvent &ev, char *err, size_t errCap);

// Peek next pending event (not committed). Returns false if empty.
bool QueuePeekPending(CapturedEvent &ev, uint32_t &slotOut);

bool QueueMarkCommitted(uint64_t eventId);
bool QueueQuarantine(uint64_t eventId, const char *reason);

QueueStats QueueGetStats();
uint64_t QueueAllocateEventId();

} // namespace gateway

#endif
