/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "term.h"
#include "pix.h"
#include "vga.h"
#include "pico/stdlib.h"
#include "tusb.h"
#include "probe.h"
#include "cdc_uart.h"
#include "led.h"
#include "get_serial.h"

int main()
{
    // Expose serial number
    usb_serial_init();

    // Bring up VGA and terminal
    vga_init();
    term_init();

    // Clear screen
    puts("\30\33[0m\f");

    // Inits
    cdc_uart_init();
    tusb_init();
    probe_gpio_init();
    probe_init();
    led_init();
    pix_init();

    while (1)
    {
        tud_task();
        cdc_task();
        probe_task();
        led_task();
        term_task();
        vga_task();
        pix_task();
    }

    return 0;
}
