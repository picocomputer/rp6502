/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The Pico SDK's interrupt API. test_vga.c keeps each handler vga.c sets, and
 * runs the backchannel's when the fake VGA's bytes arrive. */

#ifndef _TEST_HARDWARE_IRQ_H_
#define _TEST_HARDWARE_IRQ_H_

#include <stdbool.h>

typedef void (*irq_handler_t)(void);
#define PICO_DEFAULT_IRQ_PRIORITY 0x80
#define PIO0_IRQ_0 15
#define PIO0_IRQ_1 16

void irq_set_exclusive_handler(unsigned int num, irq_handler_t handler);
static inline void irq_set_priority(unsigned int num, unsigned char p) { (void)num; (void)p; }
static inline void irq_set_enabled(unsigned int num, bool e) { (void)num; (void)e; }
static inline void irq_clear(unsigned int num) { (void)num; }

#endif /* _TEST_HARDWARE_IRQ_H_ */
