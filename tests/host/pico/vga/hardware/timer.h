/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The RP2350's TIMER0 on the test's clock (pico/time.h). Its alarm, which
 * arms the VSYNC trap, never fires on the host. */

#ifndef _TEST_HARDWARE_TIMER_H_
#define _TEST_HARDWARE_TIMER_H_

#include <hardware/irq.h>
#include <pico/time.h>
#include <stdint.h>

typedef struct
{
    volatile uint32_t alarm[4];
    volatile uint32_t armed;
    volatile uint32_t timerawl;
    volatile uint32_t inte;
    volatile uint32_t intr;
} timer_hw_t;
extern timer_hw_t test_timer;
#define timer_hw (&test_timer)

static inline uint32_t time_us_32(void) { return (uint32_t)time_us_64(); }
static inline unsigned int hardware_alarm_claim_unused(bool required) { (void)required; return 0; }
static inline unsigned int hardware_alarm_get_irq_num(unsigned int alarm) { return alarm; }
static inline void hw_set_bits(volatile uint32_t *addr, uint32_t mask) { *addr |= mask; }

#endif /* _TEST_HARDWARE_TIMER_H_ */
