/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/aud/aud.h"
#include "aud/aud.h"
#include "aud/bel.h"
#define _USE_MATH_DEFINES /* MSVC: expose M_PI from <math.h> */
#include <math.h>

int8_t aud_sine_table[256];

static void (*aud_irq_fn)(void);
static uint32_t aud_irq_rate;

void aud_init(void)
{
    // Phase 0 starts at the trough (-cos), so readers can index the raw phase.
    for (unsigned i = 0; i < 256; i++)
        aud_sine_table[i] = (int8_t)lround(cos(M_PI * 2.0 / 256 * i) * -127);
    bel_setup(); // the BEL is the standing device, as on the firmware (aud.c)
}

void aud_stop(void)
{
    bel_setup();
}

void aud_setup(void (*irq_fn)(void), uint32_t rate)
{
    aud_irq_fn = irq_fn;
    aud_irq_rate = rate;
}

void (*aud_host_irq(void))(void) { return aud_irq_fn; }
uint32_t aud_host_rate(void) { return aud_irq_rate; }
