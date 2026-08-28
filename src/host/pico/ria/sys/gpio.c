/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See gpio.h.
 */

#include "ria/sys/gpio.h"
#include "ria/sys/cpu.h"
#include "ria/sys/ria.h"
#include <hardware/gpio.h>
#include <hardware/pio.h>

void __in_flash("gpio_pins_init") gpio_pins_init(void)
{
    // Hold the 6502 in reset before anything else runs.
    gpio_init(CPU_RESB_PIN);
    gpio_put(CPU_RESB_PIN, false);
    gpio_set_dir(CPU_RESB_PIN, GPIO_OUT);

    // Adjustments for GPIO performance. Important!
    // Pads and the input mux only: none of it depends on a loaded program,
    // and it has to precede every state machine that will drive these pins.
    for (int i = RIA_PIN_BASE; i < RIA_PIN_BASE + 15; i++)
    {
        pio_gpio_init(pio0, i); // any pio
        gpio_set_pulls(i, false, false);
        gpio_set_input_hysteresis_enabled(i, false);
        hw_set_bits(&pio0->input_sync_bypass, 1u << i);
        hw_set_bits(&pio1->input_sync_bypass, 1u << i);
        hw_set_bits(&pio2->input_sync_bypass, 1u << i);
    }
}
