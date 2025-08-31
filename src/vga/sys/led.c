/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <pico/stdlib.h>

// Force Pico 2 W to instead build for plain Pico 2.
// This allows for easier RIA W development without swapping cmake
// configs and prevents accidentally releasing a VGA W build.
#ifdef RASPBERRYPI_PICO2_W
#define PICO_DEFAULT_LED_PIN 25
#endif

void led_init(void)
{
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);
}
