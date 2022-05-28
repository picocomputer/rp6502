/*
 * Copyright (c) 2022 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "dm65.h"
#include "dm65.pio.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"

// Set clock for 6502 PHI2.
void dm65_set_clk_mhz(int mhz)
{
    // Only GP21 can be used for this.
    int div = 48 / mhz;
    assert(!(48 % div)); // even divisions only
    clock_gpio_init(21, clk_usb, div);
}

void dm65_init()
{
    dm65_set_clk_mhz(1);

    // Test to see if PIO can track GP21
    PIO pio = pio1;
    uint offset = pio_add_program(pio, &dm65_program);
    uint sm = pio_claim_unused_sm(pio, true);
    uint pin = 28;
    pio_sm_config c = dm65_program_get_default_config(offset);
    pio_gpio_init(pio, pin);
    sm_config_set_set_pins(&c, pin, 1);
    pio_sm_set_consecutive_pindirs(pio, sm, pin, 1, true);
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);
}
