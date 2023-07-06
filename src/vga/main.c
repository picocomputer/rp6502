/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "sys/led.h"
#include "sys/pix.h"
#include "sys/vga.h"
#include "term/font.h"
#include "term/term.h"
#include "usb/cdc.h"
#include "usb/probe.h"
#include "usb/serno.h"
#include "pico/stdlib.h"
#include "tusb.h"

static void init(void)
{
    vga_init();
    font_init();
    term_init();
    serno_init(); // before tusb
    tusb_init();
    cdc_init();
    probe_init();
    led_init();
    pix_init();
}

static void task(void)
{
    vga_task();
    term_task();
    tud_task();
    cdc_task();
    probe_task();
    led_task();
    pix_task();
}

void main_reclock(void)
{
    cdc_reclock();
    probe_reclock();
}

void main()
{
    init();
    while (1)
        task();
}
