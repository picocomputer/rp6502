/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#define _USE_MATH_DEFINES /* MSVC: expose M_PI from <math.h> */
#include "core/aud/sine.h"
#include <math.h>

int16_t sine_table[256];

void sine_init(void)
{
    for (unsigned i = 0; i < 256; i++)
        sine_table[i] = (int16_t)lround(cos(M_PI * 2.0 / 256 * i) * -32767);
}
