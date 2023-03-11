/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _API_H_
#define _API_H_

#include "mem/regs.h"
#include "mem/vram.h"
#include "mem/vstack.h"
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

void api_task();
void api_stop();
void api_reset();

// Returning data on VRAM or VSTACK requires
// ensuring the REGS have fresh data.
static inline void api_sync_vram()
{
    VRAM_RW0 = vram[VRAM_ADDR0];
    VRAM_RW1 = vram[VRAM_ADDR1];
}
static inline void api_sync_vstack()
{
    API_STACK = vstack[vstack_ptr];
}

// Return works by manipulating 10 bytes of registers.
// FFF0 EA      NOP
// FFF1 80 FE   BRA -2
// FFF3 A9 FF   LDA #$FF
// FFF5 A2 FF   LDX #$FF
// FFF7 60      RTS
// FFF8 FF FF   .SREG FF FF
static inline void api_return_blocked() { *(uint32_t *)&regs[0x10] = 0xA9FE80EA; }
static inline void api_return_released() { *(uint32_t *)&regs[0x10] = 0xA90080EA; }

// Call one of these at the very end. These signal
// the 6502 that the operation is complete.
static inline void api_return_ax(uint16_t val)
{
    *(uint32_t *)&regs[0x14] = 0x6000A200 | (val & 0xFF) | (((unsigned)val << 8) & 0xFF0000);
    api_return_released();
}
static inline void api_return_axsreg(uint32_t val)
{
    API_SREG = val >> 16;
    api_return_ax(val);
}
static inline void api_return_errno_ax(uint16_t errno, uint16_t val)
{
    API_ERRNO = errno;
    api_return_ax(val);
}
static inline void api_return_errno_axsreg(uint16_t errno, uint32_t val)
{
    API_ERRNO = errno;
    api_return_axsreg(val);
}
#endif /* _API_H_ */
