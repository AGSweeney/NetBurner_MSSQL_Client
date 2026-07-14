// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>

#ifndef GATEWAY_CONFIG_H
#define GATEWAY_CONFIG_H

#include "gateway_types.h"

namespace gateway {

void ConfigInit();
GatewayConfig &ConfigMutable();
const GatewayConfig &ConfigGet();

bool ConfigValidate(const GatewayConfig &cfg, char *err, size_t errCap);
bool ConfigSave(const GatewayConfig &cfg, char *err, size_t errCap);
bool ConfigLoad();
void ConfigSetDefaults(GatewayConfig &cfg);

bool ConfigFindPlc(uint32_t plcId, PlcConfig &out);
bool ConfigFindMapping(uint32_t mappingId, MappingConfig &out);

} // namespace gateway

#endif
