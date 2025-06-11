/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "pico.h"
#include "lwipopts.h"
#include "str.h"
#include "net/mdm.h"
#include "net/wfi.h"

#if defined(DEBUG_RIA_NET) || defined(DEBUG_RIA_NET_MDM)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__);
#else
#define DBG(...)
#endif

#define MDM_BUF_SIZE (TCP_MSS)

static char mdm_buf[MDM_BUF_SIZE];
static bool mdm_is_open;

void modem_run(void); // TODO

void mdm_task()
{
    modem_run();
}

void mdm_reset(void)
{
    mdm_is_open = false;
}

bool mdm_open(const char *filename)
{
    if (mdm_is_open)
        return false;
    while (*filename == ' ')
        filename++;
    if (!strnicmp(filename, "MODEM:", 6))
        filename += 6;
    else if (!strnicmp(filename, "AT:", 3))
        filename += 3;
    else
        return false;
    // TODO populate command buffer with filename
    mdm_is_open = true;
    return true;
}

bool mdm_close(void)
{
    if (!mdm_is_open)
        return false;
    mdm_is_open = false;
    return true;
}
