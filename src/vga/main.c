/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "sys/led.h"
#include "sys/pix.h"
#include "sys/ria.h"
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
    ria_init();
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
    ria_reclock();
}

void main_pix_cmd(uint8_t addr, uint16_t word)
{
    switch (addr)
    {
    case 0x00:
        vga_display(word);
        break;
    case 0x01:
        font_set_codepage(word);
        break;
    case 0x03:
        ria_stdout_rx(word);
        break;
    case 0x04:
        ria_backchan_req();
        break;
    case 0x05:
        ria_backchan_ack();
        break;
    case 0xFF: // TODO legacy reset, delme
        vga_display(word);
        vga_terminal(true);
        break;
    }
}

void main()
{
    init();
    while (1)
        task();
}
