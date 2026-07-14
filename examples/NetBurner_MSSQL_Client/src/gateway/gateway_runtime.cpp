// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
//
// Level-held trigger/ACK handshake runtime (OneShot §6).

#include "gateway_runtime.h"
#include "gateway_config.h"
#include "gateway_plc_adapter.h"
#include "gateway_queue.h"
#include "gateway_sql.h"

#include <cstdio>
#include <cstring>

#include <nbrtos.h>

namespace gateway {

MappingRuntime g_rt[kMaxMappings]{};
bool g_active = false;
bool g_inited = false;
uint64_t g_inflight_event_id = 0;
uint32_t g_drain_backoff_until = 0;
uint64_t g_fail_event_id = 0;
uint8_t g_fail_count = 0;

namespace {

enum { kSqlWriteFailBackoffSec = 5, kSqlWriteFailQuarantineAfter = 5 };

void SetMapErr(MappingRuntime &rt, const char *msg)
{
    ++rt.error_count;
    if (msg) {
        strncpy(rt.last_error, msg, sizeof(rt.last_error) - 1);
        rt.last_error[sizeof(rt.last_error) - 1] = '\0';
    }
    iprintf("Gateway: mapping %lu fault: %s\r\n", static_cast<unsigned long>(rt.mapping_id),
            (msg && msg[0]) ? msg : "(none)");
}

bool CapturePayload(const MappingConfig &map, const PlcConfig &plc, CapturedEvent &ev, char *err, size_t errCap)
{
    ev = CapturedEvent{};
    ev.event_id = QueueAllocateEventId();
    ev.plc_id = map.plc_id;
    ev.mapping_id = map.id;
    ev.config_revision = ConfigGet().config_revision;
    ev.capture_secs = Secs;
    ev.operation = map.operation;
    strncpy(ev.database, map.database, sizeof(ev.database) - 1);
    strncpy(ev.schema, map.schema[0] ? map.schema : "dbo", sizeof(ev.schema) - 1);
    strncpy(ev.table, map.table, sizeof(ev.table) - 1);
    ev.field_count = 0;

    for (uint16_t i = 0; i < map.field_count && i < kMaxFieldsPerMapping; ++i) {
        const FieldMapping &src = map.fields[i];
        if (!src.sql_column[0]) {
            continue;
        }
        // Skip duplicate SQL columns (corrupted saves / sticky re-add) so INSERT stays valid.
        bool dupCol = false;
        for (uint16_t j = 0; j < ev.field_count; ++j) {
            if (strcmp(ev.fields[j].sql_column, src.sql_column) == 0) {
                dupCol = true;
                break;
            }
        }
        if (dupCol) {
            continue;
        }

        const uint16_t dst = ev.field_count;
        ev.fields[dst] = src;
        if (!PlcReadField(plc, src, ev.values[dst], err, errCap)) {
            if (src.required) {
                return false;
            }
            ev.values[dst].valid = false;
            ev.values[dst].is_null = true;
        }
        ++ev.field_count;
    }

    if (map.include_plc_name && map.plc_name_column[0] && ev.field_count < kMaxFieldsPerMapping) {
        bool plcNameDup = false;
        for (uint16_t j = 0; j < ev.field_count; ++j) {
            if (strcmp(ev.fields[j].sql_column, map.plc_name_column) == 0) {
                plcNameDup = true;
                break;
            }
        }
        if (!plcNameDup) {
            FieldMapping &f = ev.fields[ev.field_count];
            FieldValue &v = ev.values[ev.field_count];
            f = FieldMapping{};
            strncpy(f.sql_column, map.plc_name_column, sizeof(f.sql_column) - 1);
            f.sql_type = SqlDataType::NVarChar;
            f.plc_type = PlcDataType::String;
            f.required = false;
            f.nullable = true;
            v = FieldValue{};
            v.valid = true;
            v.plc_type = PlcDataType::String;
            strncpy(v.text, plc.name[0] ? plc.name : "PLC", sizeof(v.text) - 1);
            ++ev.field_count;
        }
    }
    return ev.field_count > 0;
}

void StepMapping(MappingRuntime &rt, const MappingConfig &map, const PlcConfig &plc)
{
    char err[96]{};
    const uint32_t now = TimeTick;

    switch (rt.state) {
    case HandshakeState::Initializing: {
        bool trig = false;
        bool ack = false;
        const bool gotTrig = PlcReadBool(plc, map.trigger_tag, trig, err, sizeof(err));
        const bool gotAck = PlcReadBool(plc, map.ack_tag, ack, err, sizeof(err));
        if (!gotTrig || !gotAck) {
            SetMapErr(rt, err[0] ? err : "Init read failed");
            rt.state = HandshakeState::Backoff;
            rt.next_poll_tick = now + (TICKS_PER_SECOND / 2);
            return;
        }
        rt.trigger_value = trig;
        rt.ack_value = ack;
        rt.has_trigger_sample = true;
        if (!trig && !ack) {
            rt.state = HandshakeState::Idle;
        } else if (trig && !ack) {
            rt.state = HandshakeState::CapturePending;
        } else if (trig && ack) {
            rt.state = HandshakeState::WaitForTriggerLow;
        } else { // !trig && ack
            rt.state = HandshakeState::ClearAck;
        }
        break;
    }
    case HandshakeState::Idle: {
        bool trig = false;
        if (!PlcReadBool(plc, map.trigger_tag, trig, err, sizeof(err))) {
            SetMapErr(rt, err);
            rt.state = HandshakeState::Backoff;
            rt.next_poll_tick = now + (TICKS_PER_SECOND / 2);
            return;
        }
        rt.trigger_value = trig;
        if (trig) {
            rt.state = HandshakeState::Capturing;
        }
        break;
    }
    case HandshakeState::CapturePending:
        rt.state = HandshakeState::Capturing;
        break;
    case HandshakeState::Capturing: {
        static CapturedEvent s_ev;
        if (!CapturePayload(map, plc, s_ev, err, sizeof(err))) {
            ++rt.capture_retry_count;
            SetMapErr(rt, err[0] ? err : "Capture failed");
            if (rt.capture_retry_count >= map.max_capture_retries) {
                rt.capture_retry_count = 0;
                rt.state = HandshakeState::Backoff;
                rt.next_poll_tick = now + (map.capture_retry_delay_ms * TICKS_PER_SECOND) / 1000;
            }
            return;
        }
        rt.capture_retry_count = 0;
        if (!QueueAppend(s_ev, err, sizeof(err))) {
            SetMapErr(rt, err[0] ? err : "Queue full");
            // Withhold ACK; retry capture/persist later.
            rt.state = HandshakeState::Backoff;
            rt.next_poll_tick = now + (TICKS_PER_SECOND / 5);
            return;
        }
        ++rt.fire_count;
        // ACK as soon as the event is durable in the queue — do not wait for another
        // poll tick or for SqlRuntimePoll (same thread) to finish the SQL INSERT.
        if (!PlcWriteBool(plc, map.ack_tag, true, err, sizeof(err))) {
            SetMapErr(rt, err[0] ? err : "ACK high write failed");
            rt.state = HandshakeState::Backoff;
            rt.next_poll_tick = now + (TICKS_PER_SECOND / 2);
            return;
        }
        rt.ack_value = true;
        rt.state = HandshakeState::WaitForTriggerLow;
        break;
    }
    case HandshakeState::Persisting:
        rt.state = HandshakeState::AssertAck;
        break;
    case HandshakeState::AssertAck: {
        if (!PlcWriteBool(plc, map.ack_tag, true, err, sizeof(err))) {
            SetMapErr(rt, err[0] ? err : "ACK high write failed");
            rt.state = HandshakeState::Backoff;
            rt.next_poll_tick = now + (TICKS_PER_SECOND / 2);
            return;
        }
        rt.ack_value = true;
        rt.state = HandshakeState::WaitForTriggerLow;
        break;
    }
    case HandshakeState::WaitForTriggerLow: {
        bool trig = false;
        if (!PlcReadBool(plc, map.trigger_tag, trig, err, sizeof(err))) {
            SetMapErr(rt, err);
            return;
        }
        rt.trigger_value = trig;
        if (!trig) {
            rt.state = HandshakeState::ClearAck;
        }
        break;
    }
    case HandshakeState::ClearAck: {
        if (!PlcWriteBool(plc, map.ack_tag, false, err, sizeof(err))) {
            SetMapErr(rt, err[0] ? err : "ACK low write failed");
            rt.state = HandshakeState::Backoff;
            rt.next_poll_tick = now + (TICKS_PER_SECOND / 2);
            return;
        }
        rt.ack_value = false;
        rt.state = HandshakeState::Idle;
        break;
    }
    case HandshakeState::VerifyAckLow:
        rt.state = HandshakeState::Idle;
        break;
    case HandshakeState::Backoff:
        if (static_cast<int32_t>(now - rt.next_poll_tick) >= 0) {
            rt.state = HandshakeState::Initializing;
        }
        break;
    case HandshakeState::Faulted:
        break;
    }
}

} // namespace

void RuntimeReloadFromConfig()
{
    memset(g_rt, 0, sizeof(g_rt));
    const GatewayConfig &cfg = ConfigGet();
    for (uint16_t i = 0; i < cfg.mapping_count && i < kMaxMappings; ++i) {
        g_rt[i].mapping_id = cfg.mappings[i].id;
        g_rt[i].state = HandshakeState::Initializing;
        g_rt[i].next_poll_tick = TimeTick + (i * (TICKS_PER_SECOND / 20)); // stagger
    }
    g_active = cfg.enabled;
}

void RuntimeInit()
{
    if (g_inited) {
        return;
    }
    ConfigInit();
    QueueInit();
    RuntimeReloadFromConfig();
    g_inited = true;
}

void RuntimeShutdown()
{
    g_active = false;
    QueueShutdown();
    g_inited = false;
}

bool RuntimeIsActive()
{
    return g_active;
}

void RuntimeSetActive(bool active)
{
    g_active = active;
    GatewayConfig &cfg = ConfigMutable();
    cfg.enabled = active;
}

MappingRuntime *RuntimeFind(uint32_t mappingId)
{
    for (int i = 0; i < kMaxMappings; ++i) {
        if (g_rt[i].mapping_id == mappingId) {
            return &g_rt[i];
        }
    }
    return nullptr;
}

const MappingRuntime *RuntimeFindConst(uint32_t mappingId)
{
    return RuntimeFind(mappingId);
}

void RuntimePoll()
{
    if (!g_inited || !g_active) {
        return;
    }

    const GatewayConfig &cfg = ConfigGet();
    const uint32_t now = TimeTick;

    // Fair round-robin: one due mapping per poll call.
    static uint16_t s_rr = 0;
    for (uint16_t n = 0; n < cfg.mapping_count; ++n) {
        const uint16_t i = static_cast<uint16_t>((s_rr + n) % (cfg.mapping_count ? cfg.mapping_count : 1));
        if (i >= cfg.mapping_count) {
            break;
        }
        const MappingConfig &map = cfg.mappings[i];
        if (!map.enabled) {
            continue;
        }
        MappingRuntime &rt = g_rt[i];
        if (rt.mapping_id != map.id) {
            rt = MappingRuntime{};
            rt.mapping_id = map.id;
            rt.state = HandshakeState::Initializing;
        }

        PlcConfig plc{};
        if (!ConfigFindPlc(map.plc_id, plc) || !plc.enabled) {
            continue;
        }

        const uint32_t intervalTicks =
            (plc.poll_interval_ms * TICKS_PER_SECOND) / 1000u;
        const uint32_t minInterval = intervalTicks ? intervalTicks : (TICKS_PER_SECOND / 5);

        // Active handshake states get priority / shorter cadence
        const bool urgent = (rt.state != HandshakeState::Idle && rt.state != HandshakeState::Backoff &&
                             rt.state != HandshakeState::Faulted);
        if (!urgent && static_cast<int32_t>(now - rt.next_poll_tick) < 0) {
            continue;
        }

        rt.last_poll_tick = now;
        rt.next_poll_tick = now + minInterval;
        StepMapping(rt, map, plc);
        s_rr = static_cast<uint16_t>(i + 1);
        break; // one mapping work unit per poll for fairness
    }

    // Ask SQL side to drain when not busy (respect write-fail backoff).
    if (!SqlRuntimeIsBusy() && static_cast<int32_t>(TimeTick - g_drain_backoff_until) >= 0) {
        char err[96]{};
        SqlDrainOnePending(err, sizeof(err));
    }
}

void NotifySqlCommitted(uint64_t eventId)
{
    QueueMarkCommitted(eventId);
    g_inflight_event_id = 0;
    g_fail_event_id = 0;
    g_fail_count = 0;
    g_drain_backoff_until = 0;
}

void NotifySqlWriteFailed()
{
    const uint64_t eid = g_inflight_event_id;
    g_inflight_event_id = 0;
    g_drain_backoff_until = TimeTick + (kSqlWriteFailBackoffSec * TICKS_PER_SECOND);

    if (eid == 0) {
        return;
    }

    if (eid == g_fail_event_id) {
        ++g_fail_count;
    } else {
        g_fail_event_id = eid;
        g_fail_count = 1;
    }

    if (g_fail_count >= kSqlWriteFailQuarantineAfter) {
        QueueQuarantine(eid, "SQL write failed repeatedly");
        iprintf("Gateway: quarantined event %llu after repeated SQL write failures\r\n",
                static_cast<unsigned long long>(eid));
        g_fail_event_id = 0;
        g_fail_count = 0;
    }
}

uint64_t TakeInflightEventId()
{
    return g_inflight_event_id;
}

bool HasInflightEvent()
{
    return g_inflight_event_id != 0;
}

void SetInflightEventId(uint64_t id)
{
    g_inflight_event_id = id;
}

} // namespace gateway
