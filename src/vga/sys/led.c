/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <pico.h>
#include <pico/stdlib.h>

// Force RP6502 VGA to always build as plain Pico 2.
// This prevents accidentally releasing a "W" build.
#ifdef RASPBERRYPI_PICO2_W
#define PICO_DEFAULT_LED_PIN 25
#endif

void led_init(void)
{
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);
}
