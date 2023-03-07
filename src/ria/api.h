/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _API_H_
#define _API_H_

#include "mem/regs.h"
#include <stdint.h>

#define API_OP REGS(0xFFEF)
#define API_ERRNO REGSW(0xFFED)
#define API_STACK REGS(0xFFEC)
#define API_BUSY (REGS(0xFFF2) & 0x80)
#define API_A REGS(0xFFF4)
#define API_X REGS(0xFFF6)
#define API_SREG REGSW(0xFFF8)
#define API_AX (API_A | (API_X << 8))
#define API_AXSREG (API_AX | (API_SREG << 16))

// Returning data on VRAM or VSTACK requires
// ensuring the REGS have fresh data.
#define API_RETURN_VRAM()            \
    {                                \
        VRAM_RW0 = vram[VRAM_ADDR0]; \
        VRAM_RW1 = vram[VRAM_ADDR1]; \
    }
#define API_RETURN_VSTACK() \
    API_STACK = vstack[vstack_ptr];

// Return works by manipulating 10 bytes of registers.
// FFF0 EA      NOP
// FFF1 80 FE   BRA -2
// FFF3 A9 FF   LDA #$FF
// FFF5 A2 FF   LDX #$FF
// FFF7 60      RTS
// FFF8 FF FF   .SREG FF FF
#define API_SPIN_LOCK() *(uint32_t *)&regs[0x10] = 0xA9FE80EA
#define API_SPIN_RELEASE() *(uint32_t *)&regs[0x10] = 0xA90080EA

// Call one of these at the very end. These signal
// the 6502 that the operation is complete.
// There is no API_RETURN_A because CC65 requires a
// minimum of AX to support interger promotion.
// 16-bit returns
#define API_RETURN_AX(val)                                                                        \
    {                                                                                             \
        *(uint32_t *)&regs[0x14] = 0x6000A200 | (val & 0xFF) | (((unsigned)val << 8) & 0xFF0000); \
        API_SPIN_RELEASE();                                                                       \
    }
// 32-bit returns
#define API_RETURN_AXSREG(val)           \
    {                                    \
        API_SREG = (val >> 16) & 0xFFFF; \
        API_RETURN_AX(val);              \
    }

void api_task();
void api_stop();
void api_reset();

#endif /* _API_H_ */
