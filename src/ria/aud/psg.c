/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "aud/aud.h"
#include "aud/psg.h"

static uint16_t psg_xaddr;

static void psg_start(void)
{
}

static void psg_stop(void)
{
}

static void psg_reclock(uint32_t sys_clk_khz)
{
}

static void psg_task(void)
{
}

bool psg_xreg(uint16_t word)
{

    if (word > 0x10000 - 99) // TODO
    {
        psg_xaddr = 0xFFFF;
    }
    else
    {
        psg_xaddr = word;
        void aud_setup(psg_start, psg_stop, psg_reclock, psg_task);
    }
    return true;
}
