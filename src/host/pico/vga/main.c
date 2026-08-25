/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "vga/main.h"
#include "core/vga/mode0.h"
#include "core/vga/mode1.h"
#include "core/vga/mode2.h"
#include "core/vga/mode3.h"
#include "core/vga/mode4.h"
#include "core/vga/mode5.h"
#include "vga/sys/com.h"
#include "vga/sys/led.h"
#include "vga/sys/pix.h"
#include "vga/sys/ria.h"
#include "vga/sys/sys.h"
#include "vga/sys/vga.h"
#include "core/term/font.h"
#include "core/term/term.h"
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
    return vga_mode_prog(xregs[1], xregs);
}

int main(void)
{
    init();
    while (1)
        task();
}
