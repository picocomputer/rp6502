/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sys/led.h"
#include "pico/stdlib.h"
#ifdef RASPBERRYPI_PICO_W
#include "pico/cyw43_arch.h"
#endif

void led_init(void)
{
    // Turn on the Pi Pico LED
#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);
#endif
#ifdef RASPBERRYPI_PICO_W
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
#endif
}
