/*
 * Copyright (c) 2022 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "dm65.h"
#include "vga.h"
#include "mon.h"
#include "term.h"
#include "hid.h"
#include "pico/stdlib.h"
#include "_tinyusb/tusb.h"

int main()
{
    // Pi Pico LED on.
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    // Bring up VGA and terminal
    vga_init();
    term_init();

    // Serial monitor on (after VGA changes clock)
    stdio_init_all();

    // Hello, world.
    puts("\30\33[0m\f\nPicocomputer 6502 \33[31mC\33[32mO\33[33mL\33[36mO\33[35mR\33[0m\n");

    // TinyUSB host support for keyboards,
    // mice, joysticks, and storage devices.
    tusb_init();
    hid_init();

    // DMA to 65C02 uses the PIO and a handful of 74HC logic
    // chips for direct memory access and debugging.
    dm65_init();

    while (1)
    {
        tuh_task();
        hid_task();
        mon_task();
        term_task();
    }

    return 0;
}
