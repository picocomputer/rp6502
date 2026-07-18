/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "vga/main.h"
#include "vga/modes/mode1.h"
#include "vga/modes/mode2.h"
#include "vga/modes/mode3.h"
#include "vga/modes/mode4.h"
#include "vga/modes/mode5.h"
#include "vga/sys/com.h"
#include "vga/sys/led.h"
#include "vga/sys/pix.h"
#include "vga/sys/ria.h"
#include "vga/sys/sys.h"
#include "vga/sys/vga.h"
#include "vga/term/font.h"
#include "vga/term/term.h"
#include "vga/usb/cdc.h"
#include "vga/usb/usb.h"
#include <pico/stdlib.h>

static void init(void)
{
    com_init();
    vga_init();
    font_init();
    term_init();
    usb_init();
    led_init();
    ria_init();
    pix_init();
}

static void task(void)
{
    // com_task is important
    term_task();
    com_task();
    cdc_task();
    com_task();
    ria_task();
    com_task();
    vga_task();
    com_task();
    usb_task();
    com_task();
    pix_task();
    com_task();
    sys_task();
    com_task();
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
    case 5:
        return mode5_prog(xregs);
    default:
        return false;
    }
}

int main(void)
{
    init();
    while (1)
        task();
}
