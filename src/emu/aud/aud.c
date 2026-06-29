/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Emulator stand-in for ria/aud/aud.c (named for the firmware component it
 * replaces, like ria.c/vga.c). The firmware aud.c drives the RP2350 PWM and a
 * sample-rate interrupt; the desktop has neither. This keeps the hardware-
 * independent half — the shared sine table and the one-active-device manager —
 * and, instead of wiring an IRQ, records the installed sample handler and rate
 * so snd.c can pump it. The vendored psg/opl/bel drivers link against this
 * exactly as they would the firmware aud.c.
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
    // Match the firmware: stopping a PSG/OPL device falls back to the standing
    // BEL device (silent until rung), not to no device at all.
    bel_setup();
}

void aud_setup(void (*irq_fn)(void), uint32_t rate)
{
    aud_irq_fn = irq_fn;
    aud_irq_rate = rate;
}

void (*aud_host_irq(void))(void) { return aud_irq_fn; }
uint32_t aud_host_rate(void) { return aud_irq_rate; }
