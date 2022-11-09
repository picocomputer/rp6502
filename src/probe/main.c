/*
 * Copyright (c) 2022 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "../term.h"
#include "../vga.h"
#include "pico/stdlib.h"
#include "tusb.h"
#include "probe.h"
#include "cdc_uart.h"
#include "led.h"

int main()
{
    // Bring up VGA and terminal
    vga_init();
    term_init();

    // Clear screen
    puts("\30\33[0m\f");

    // Inits
    cdc_uart_init();
    tusb_init();
    probe_init();
    led_init();

    while (1)
    {
        tud_task();
        cdc_task();
        probe_task();
        led_task();
        term_task();
        vga_task();
    }

    return 0;
}
