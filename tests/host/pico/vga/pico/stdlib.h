/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* What vga.c takes of the Pico SDK's stdlib: the clock, and the GPIO
 * functions, which have no pins to set on the host. */

#ifndef _TEST_PICO_STDLIB_H_
#define _TEST_PICO_STDLIB_H_

#include <hardware/pio.h>
#include <hardware/timer.h>
#include <pico/time.h>
#include <stdbool.h>

enum gpio_function
{
    GPIO_FUNC_UART,
};

static inline void gpio_set_function(unsigned int pin, enum gpio_function fn) { (void)pin; (void)fn; }
static inline void gpio_pull_up(unsigned int pin) { (void)pin; }

#endif /* _TEST_PICO_STDLIB_H_ */
