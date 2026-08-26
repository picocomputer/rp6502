/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What every machine's audio shares. The device behind it is not shared --
 * PWM on a Pico, a host sink in the emulator, fabric on a Pocket -- but the
 * table the voices read is one table, and it was being built twice.
 */

#define _USE_MATH_DEFINES /* MSVC: expose M_PI from <math.h> */
#include "core/aud/aud.h"
#include <math.h>

int16_t aud_sine_table[256];

void aud_sine_init(void)
{
    // Phase 0 starts at the trough (-cos), so readers can index the raw phase.
    for (unsigned i = 0; i < 256; i++)
        aud_sine_table[i] = (int16_t)lround(cos(M_PI * 2.0 / 256 * i) * -32767);
}
