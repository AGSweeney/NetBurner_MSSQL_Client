// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
//
// Micro800→MSSQL gateway types (level-held trigger/ACK; see Micro800_MSSQL_Gateway_OneShot.md).

#ifndef GATEWAY_TYPES_H
#define GATEWAY_TYPES_H

#include <cstddef>
#include <cstdint>

#include <nettypes.h>

#include "sql_runtime.h"

namespace gateway {

enum {
    kMaxPlcs = 4,
    kMaxMappings = 8,
    kMaxMappingsPerPlc = 4,
    // Keep field arrays small: MappingConfig/CapturedEvent must not be stack-allocated on NB HTTP task.
    kMaxFieldsPerMapping = 16,
    kMaxTagLen = 64,
    kMaxNameLen = 32,
    kMaxSqlIdentLen = 64,
    kMaxStringValueLen = 64,
    kMaxSerializedEvent = 1024,
    // EFFS queue budget (segment payloads). RAM fallback still uses kMaxQueueRecords.
    kQueueBudgetBytesDefault = 512 * 1024,
    kQueueSegmentBytes = 64 * 1024,
    kMaxQueueRecords = 64,
};

enum class HandshakeState : uint8_t {
    Initializing = 0,
    Idle,
    CapturePending,
    Capturing,
    Persisting,
    AssertAck,
    WaitForTriggerLow,
    ClearAck,
    VerifyAckLow,
    Backoff,
    Faulted
};

enum class PlcDataType : uint8_t {
    Bool = 0,
    Sint = 1,
    Int = 2,
    Dint = 3,
    Real = 4,
    String = 5,
    // Keep Unknown=6 for configs saved before unsigned/64-bit types existed.
    Unknown = 6,
    Usint = 7,
    Uint = 8,
    Udint = 9,
    Lint = 10,
    Ulint = 11,
    Lreal = 12,
};

enum class SqlDataType : uint8_t {
    Bit = 0,
    TinyInt = 1,
    SmallInt = 2,
    Int = 3,
    Real = 4,
    Float = 5,
    NVarChar = 6,
    VarChar = 7,
    // Keep Unknown=8 for older saved configs.
    Unknown = 8,
    BigInt = 9,
};

enum class SqlOperation : uint8_t {
    Insert = 0,
    Update = 1,
};

enum class IdempotencyMode : uint8_t {
    None = 0,
    DestinationEventIdColumn,
    GatewayLedgerTable,
    StoredProcedure,
};

struct PlcConfig {
    uint32_t id{0};
    bool enabled{false};
    char name[kMaxNameLen]{};
    IPADDR4 ip{};
    uint32_t poll_interval_ms{200};
    uint32_t request_timeout_ms{3000};
    uint32_t retry_delay_ms{500};
};

struct FieldMapping {
    char plc_tag[kMaxTagLen]{};
    PlcDataType plc_type{PlcDataType::Unknown};
    char sql_column[kMaxSqlIdentLen]{};
    SqlDataType sql_type{SqlDataType::Unknown};
    bool required{true};
    bool nullable{false};
    bool is_where_key{false}; // UPDATE key
    double scale{1.0};
    double offset{0.0};
    uint16_t max_string_length{kMaxStringValueLen};
};

struct MappingConfig {
    uint32_t id{0};
    uint32_t plc_id{0};
    bool enabled{false};
    char name[kMaxNameLen]{};
    char trigger_tag[kMaxTagLen]{};
    char ack_tag[kMaxTagLen]{};
    SqlOperation operation{SqlOperation::Insert};
    char database[SQL_CFG_DATABASE_LEN]{};
    char schema[32]{"dbo"};
    char table[SQL_CFG_TABLE_LEN]{};
    uint16_t field_count{0};
    FieldMapping fields[kMaxFieldsPerMapping]{};
    uint32_t max_capture_retries{3};
    uint32_t capture_retry_delay_ms{250};
    // When set, capture stamps PlcConfig.name into this SQL column (no PLC tag read).
    bool include_plc_name{false};
    char plc_name_column[kMaxSqlIdentLen]{};
};

struct GatewayConfig {
    bool enabled{false};
    IdempotencyMode idempotency{IdempotencyMode::None};
    char event_id_column[kMaxSqlIdentLen]{"GatewayEventId"};
    uint16_t plc_count{0};
    uint16_t mapping_count{0};
    PlcConfig plcs[kMaxPlcs]{};
    MappingConfig mappings[kMaxMappings]{};
    uint32_t config_revision{0};
};

struct FieldValue {
    bool valid{false};
    bool is_null{false};
    PlcDataType plc_type{PlcDataType::Unknown};
    // Numeric / bool storage
    int64_t i64{0};
    float f32{0.f};
    char text[kMaxStringValueLen]{};
};

struct CapturedEvent {
    uint64_t event_id{0};
    uint32_t plc_id{0};
    uint32_t mapping_id{0};
    uint32_t config_revision{0};
    uint32_t capture_secs{0};
    SqlOperation operation{SqlOperation::Insert};
    char database[SQL_CFG_DATABASE_LEN]{};
    char schema[32]{};
    char table[SQL_CFG_TABLE_LEN]{};
    uint16_t field_count{0};
    FieldMapping fields[kMaxFieldsPerMapping]{};
    FieldValue values[kMaxFieldsPerMapping]{};
};

struct MappingRuntime {
    uint32_t mapping_id{0};
    HandshakeState state{HandshakeState::Initializing};
    uint32_t next_poll_tick{0};
    uint32_t last_poll_tick{0};
    uint32_t capture_retry_count{0};
    uint32_t consecutive_comm_failures{0};
    bool trigger_value{false};
    bool ack_value{false};
    bool has_trigger_sample{false};
    uint32_t fire_count{0};
    uint32_t error_count{0};
    char last_error[96]{};
};

const char *HandshakeStateName(HandshakeState s);

} // namespace gateway

#endif
