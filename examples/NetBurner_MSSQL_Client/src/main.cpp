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

#include "sql_nv.h"
#include "sql_runtime.h"
#include "sql_results.h"

const char *AppName = "NetBurner_MSSQL_Client";

extern "C" void UserMain(void *pd)
{
    (void) pd;

    init();
    SqlNvInit();
    SqlRuntimeInitDefaults();
    SqlResultsReset();
    StartHttp();
    WaitForActiveNetwork(TICKS_PER_SECOND * 15);
    EnableRemoteConsole();
    SqlRuntimeInit();

    while (1) {
        SqlRuntimePoll();
        OSTimeDly(TICKS_PER_SECOND / 10);
    }
}
