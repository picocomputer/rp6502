/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "vga/main.h"
#include "drivers.h"
#include "core/vga/mode/mode0.h"
#include "core/vga/mode/mode1.h"
#include "core/vga/mode/mode2.h"
#include "core/vga/mode/mode3.h"
#include "core/vga/mode/mode4.h"
#include "core/vga/mode/mode5.h"
#include <pico/stdlib.h>

static void init(void)
{
#define DRIVER(i, t, iot, r, s, b, ...) i();
    DRIVERS_FORWARD(RP6502_MACH_DRIVERS)
#undef DRIVER
    /* Core 1 renders scanlines with vga_render_scanline, so it must not start
     * before ria_init, term_init, font_init and vga_init have run. Until then,
     * ria_lock is null, mutex_init has not run on vga_scanline_mutex, the
     * terminal's screen pointer is null and the glyph tables are
     * uninitialized RAM. */
    vga_start_render_core();
}

/* com_task runs after every driver's task because the UART's RX FIFO holds
 * only 32 bytes and overruns if a slow driver delays the next call. */
static void task(void)
{
#define DRIVER(i, t, iot, r, s, b, ...) t(); iot(); com_task();
    DRIVERS_FORWARD(RP6502_MACH_DRIVERS)
#undef DRIVER
}

void main_pre_reclock(void)
{
    ria_pre_reclock();
    com_pre_reclock();
}

void main_post_reclock(void)
{
    ria_post_reclock();
    com_post_reclock();
}

bool main_prog(uint16_t *xregs)
{
    return vga_mode_prog(xregs[1], xregs);
}

int main(void)
{
    init();
    while (1)
        task();
}
