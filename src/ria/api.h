/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _API_H_
#define _API_H_

#include "mem/vstack.h"
#include "mem/regs.h"
#include <stdint.h>

#define VRAM_RW0 REGS(0xFFE4)
#define VRAM_STEP0 (int8_t) REGS(0xFFE5)
#define VRAM_ADDR0 REGSW(0xFFE6)

#define VRAM_RW1 REGS(0xFFE8)
#define VRAM_STEP1 (int8_t) REGS(0xFFE9)
#define VRAM_ADDR1 REGSW(0xFFEA)

extern volatile uint16_t vram_ptr0;
extern volatile uint16_t vram_ptr1;

#define API_OPCODE REGS(0xFFEF)
#define API_AX REGSW(0xFFED)
#define API_STACK_RW REGSW(0xFFEC)
#define API_RETURN(val)                   \
    {                                     \
        API_OPCODE |= 0x80;               \
        vstack_ptr = VSTACK_SIZE;         \
        REGS(0xFFF5) = (val >> 8) & 0xFF; \
        REGS(0xFFF3) = val & 0xFF;        \
        REGS(0xFFF1) = 0;                 \
    }

void api_task();
void api_reset();

#endif /* _API_H_ */
