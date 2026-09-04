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
    /* Last, and not inside vga_init where it used to live: core 1 renders
     * the terminal, and until everything above has run there is no terminal
     * to render -- the screen pointer is null and the glyph tables are
     * uninitialized RAM. */
    vga_start_render_core();
}

/* com_task after every driver, which is why this machine expands the walk
 * itself. The UART is this firmware's whole reason to exist and its FIFO is
 * 32 bytes; a driver that takes its time would overrun it. Both columns,
 * because the terminal's row is core's and carries term_task in the io one --
 * there is no file IO here to split them apart. */
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
