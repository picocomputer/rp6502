/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "pico.h"

#ifndef RP6502_RIA_W
void mdm_task() {}
#else

#include "net/mdm.h"
#include "net/wfi.h"

#if defined(DEBUG_RIA_NET) || defined(DEBUG_RIA_NET_MDM)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__);
#else
#define DBG(...)
#endif

void modem_run(void);

void mdm_task()
{
    modem_run();
}

#endif /* RP6502_RIA_W */
