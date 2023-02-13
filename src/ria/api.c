/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api.h"
#include "mem/regs.h"
#include "mem/vram.h"
#include "mem/vstack.h"

#include "stdio.h" //TEMP
#include "pico/time.h" //TEMP

volatile uint16_t vram_ptr0;
volatile uint16_t vram_ptr1;

void api_task()
{
    switch (API_OPCODE) // 1-127 valid
    {
    case 1: // open0
        printf(">open0> %d %s\n", API_AX, &vram[vram_ptr0]);
        API_RETURN(7);
        break;
    }
}

void api_reset()
{
    vstack_ptr = VSTACK_SIZE;
}
