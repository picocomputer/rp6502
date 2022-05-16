/*
 * Copyright (c) 2022 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "dm65.h"
#include "dm65.pio.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"

void dm65_init()
{
    // Divide the USB 48 MHz clock for 6502 PHI2.
    // Only GP21 is exposed for this on the Pi Pico board.
    clock_gpio_init(21, clk_usb, 6); // 8 MHZ
    // clock_gpio_init(21, clk_usb, 12); // 4 MHZ
    // clock_gpio_init(21, clk_usb, 24); // 2 MHZ
    // clock_gpio_init(21, clk_usb, 48); // 1 MHZ

    // Test to see if PIO can track GP21
    PIO pio = pio1;
    uint offset = pio_add_program(pio, &hello_program);
    uint sm = pio_claim_unused_sm(pio, true);
    uint pin = 28;
    pio_sm_config c = hello_program_get_default_config(offset);
    pio_gpio_init(pio, pin);
    sm_config_set_set_pins(&c, pin, 1);
    pio_sm_set_consecutive_pindirs(pio, sm, pin, 1, true);
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);
}
