// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>

#ifndef GATEWAY_PLC_ADAPTER_H
#define GATEWAY_PLC_ADAPTER_H

#include "gateway_types.h"

namespace gateway {

bool PlcReadBool(const PlcConfig &plc, const char *tag, bool &value, char *err, size_t errCap);
bool PlcWriteBool(const PlcConfig &plc, const char *tag, bool value, char *err, size_t errCap);
bool PlcReadField(const PlcConfig &plc, const FieldMapping &field, FieldValue &out, char *err, size_t errCap);

PlcDataType PlcTypeFromSymbol(uint16_t symbolType);
bool ProbePlc(const PlcConfig &plc, char *err, size_t errCap);

} // namespace gateway

#endif
