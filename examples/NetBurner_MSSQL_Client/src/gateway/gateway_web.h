// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>

#ifndef GATEWAY_WEB_H
#define GATEWAY_WEB_H

#ifdef NB_GATEWAY_MICRO800

void ShowAppNav(int sock, const char *url);
void ShowGatewayNavLinks(int sock, const char *url);
void ShowGatewayOverview(int sock, const char *url);
void ShowPlcManager(int sock, const char *url);
void ShowSqlMapManager(int sock, const char *url);
void ShowSqlMapsAutoRefresh(int sock, const char *url);

void GatewayWebInit();

#endif

#endif
