/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * A firmware.bin runs only under the bitstream it was built for. The
 * bitstream shows its register layout version and its savestate version
 * in two registers, this image carries its own two after its build stamp,
 * and crt0.S calls ver_check before anything else uses a register.
 *
 * When either differs the firmware stops here for good. The screen then
 * shows a blinking cursor and nothing else, and that is the whole error:
 * no register can be trusted to say more.
 */

#include "mmio.h"

#include "core/term/color.h"

/* This image's versions, in crt0.S. */
extern const uint32_t ver_own[2];

/* The video device shows nothing until it has a scanline program, so the
 * stop gives it the least that draws a cursor: the console's first row,
 * 16 scanlines of mode 0 on plane 0 of the power-on canvas, and a block
 * cursor at its start in the console's colour. The cursor is lit for 30
 * frames and dark for 30, as the terminal blinks it. This runs before
 * crt0.S has made the data, so it uses constants alone. */
static void ver_stop(void)
{
    for (uint32_t line = 0; line < 16; line++)
        VID_XPROG(line, 0, 0) = 0x80000000u;
    VID_PROG = 0x80000000u | (16u << 16);
    VID_CURSOR_COLOR = color_256[7];
    for (;;)
        VID_CURSOR = (1u << 25) | ((uint32_t)!((VID_FRAME / 30u) & 1u) << 24);
}

void ver_check(void);

void ver_check(void)
{
    if (VER_REG != ver_own[0] || VER_SST != ver_own[1])
        ver_stop();
}
