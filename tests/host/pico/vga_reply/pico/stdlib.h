/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* What the VGA's ria.c takes of the Pico SDK's stdlib: the clock, the GPIO
 * functions, which have no pins to set on the host, and the UART that
 * answers the RIA's request, which test_vga_reply.c does not ask. */

#ifndef _TEST_PICO_STDLIB_H_
#define _TEST_PICO_STDLIB_H_

#include <hardware/pio.h>
#include <hardware/timer.h>
#include <pico/time.h>
#include <stdbool.h>
#include <stddef.h>

enum gpio_function
{
    GPIO_FUNC_UART,
};

static inline void gpio_set_function(unsigned int pin, enum gpio_function fn) { (void)pin; (void)fn; }
static inline void gpio_pull_up(unsigned int pin) { (void)pin; }

typedef struct uart_inst uart_inst_t;
#define uart1 ((uart_inst_t *)0)
static inline void uart_write_blocking(uart_inst_t *uart, const uint8_t *src, size_t len)
{
    (void)uart; (void)src; (void)len;
}

static inline void busy_wait_us_32(uint32_t us) { (void)us; }

#endif /* _TEST_PICO_STDLIB_H_ */
