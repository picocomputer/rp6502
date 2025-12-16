/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sys/led.h"
#include "net/cyw.h"
#include <pico/stdlib.h>

#if defined(DEBUG_RIA_SYS) || defined(DEBUG_RIA_SYS_LED)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define LED_BLINK_TIME_MS 100

bool led_state;
bool led_blinking;
absolute_time_t led_blink_timer;

static void led_set(bool on)
{
    led_state = on;
#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, on);
#endif
#ifdef RP6502_RIA_W
    // LED is connected to cyw43
    cyw_led_set(on);
#endif
}

void led_init(void)
{
    led_set(true);
}

void led_task(void)
{
    if (led_blinking && absolute_time_diff_us(get_absolute_time(), led_blink_timer) < 0)
    {
        led_state = !led_state;
        led_set(led_state);
        led_blink_timer = make_timeout_time_ms(LED_BLINK_TIME_MS);
    }
}

void led_blink(bool on)
{
    if (!on)
        led_set(true);
    led_blinking = on;
}
