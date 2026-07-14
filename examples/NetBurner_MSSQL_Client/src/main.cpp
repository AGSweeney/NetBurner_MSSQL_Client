// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>

/* Revision: 1.0.0 */

/******************************************************************************
* NetBurner MSSQL Client for NANO54415
*
* Browse to http://<device-ip>/ to configure SQL connection and run queries.
******************************************************************************/

#include <init.h>
#include <nbrtos.h>
#include <remoteconsole.h>
#include <stdio.h>

#include "sql_nv.h"
#include "sql_runtime.h"
#include "sql_results.h"

#ifdef NB_GATEWAY_MICRO800
#include "gateway/gateway_runtime.h"
#include "gateway/gateway_web.h"
#endif

const char *AppName = "NetBurner_MSSQL_Client";

extern "C" void Local_OutString(const char *s);

namespace {

// Prefer local UART write for boot breadcrumbs: avoids stdio lock stalls if
// HTTP/remote-console tasks contend for the console during bring-up.
void BootMsg(const char *msg)
{
    if (!msg) {
        return;
    }
#if defined(NB_GATEWAY_MICRO800)
    Local_OutString(msg);
    Local_OutString("\r\n");
#else
    iprintf("%s\r\n", msg);
#endif
}

} // namespace

extern "C" void UserMain(void *pd)
{
    (void) pd;

    init();
    SqlNvInit();
    SqlRuntimeInitDefaults();
    SqlResultsReset();

    // Classic NetBurner order: wait for IP before starting HTTP/console.
    // Starting those first raced with DHCP and hung mid-print on gateway builds.
    BootMsg("Waiting for network");
    WaitForActiveNetwork(TICKS_PER_SECOND * 15);
    BootMsg("Network active");

    StartHttp();
    BootMsg("HTTP started");
    EnableRemoteConsole();
    BootMsg("Remote console enabled");

#ifdef NB_GATEWAY_MICRO800
    BootMsg("Gateway init...");
    gateway::RuntimeInit();
    GatewayWebInit();
    BootMsg(gateway::RuntimeIsActive() ? "Gateway active (RAM queue)" : "Gateway idle");
#endif
    SqlRuntimeInit();
    BootMsg("Ready");

    while (1) {
#ifdef NB_GATEWAY_MICRO800
        // Gateway handshake before SQL: ACK must not wait behind a blocking INSERT
        // on this same UserMain thread.
        gateway::RuntimePoll();
#endif
        SqlRuntimePoll();
        OSTimeDly(TICKS_PER_SECOND / 10);
    }
}
