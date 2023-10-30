/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "modes/mode1.h"
#include "modes/mode2.h"
#include "modes/mode3.h"
#include "modes/mode4.h"
#include "sys/led.h"
#include "sys/pix.h"
#include "sys/ria.h"
#include "sys/std.h"
#include "sys/vga.h"
#include "term/font.h"
#include "term/term.h"
#include "usb/cdc.h"
#include "usb/serno.h"
#include "pico/stdlib.h"
#include "tusb.h"

static void init(void)
{
    std_init();
    vga_init();
    font_init();
    term_init();
    serno_init(); // before tusb
    tusb_init();
    led_init();
    ria_init();
    pix_init();
}

static void task(void)
{
    vga_task();
    term_task();
    tud_task();
    cdc_task();
    pix_task();
    ria_task();
    std_task();
}

void main_flush(void)
{
    ria_flush();
}

void main_reclock(void)
{
    std_reclock();
    ria_reclock();
}

bool main_prog(uint16_t *xregs)
{
    switch (xregs[1])
    {
    case 0:
        return term_prog(xregs);
    case 1:
        return mode1_prog(xregs);
    case 2:
        return mode2_prog(xregs);
    case 3:
        return mode3_prog(xregs);
    case 4:
        return mode4_prog(xregs);
    default:
        return false;
    }
}

void main()
{
    init();
    while (1)
        task();
}
