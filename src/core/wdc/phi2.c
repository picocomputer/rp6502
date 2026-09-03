/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * PHI2 on a software machine, which is a number and nothing else.
 *
 * There is no divider here. The board has one because a PIO clock divider is
 * what makes its PHI2, and the fabric has one because a clock enable is what
 * makes its own -- see phi2.sv. Here the beam is the machine's clock and the
 * bus converts scanlines into cycles when it needs to, so every whole
 * kilohertz in range is exact for the same reason it is exact in fabric:
 * nothing rounds.
 */

#include "core/wdc/phi2.h"
#include "core/sys/config.h"

static uint16_t khz_run = PHI2_DEFAULT_KHZ;

void phi2_set_khz_run(uint16_t khz)
{
    if (khz < PHI2_MIN_KHZ)
        khz = PHI2_MIN_KHZ;
    if (khz > PHI2_MAX_KHZ)
        khz = PHI2_MAX_KHZ;
    khz_run = khz;
}

uint16_t phi2_get_khz_run(void)
{
    return khz_run;
}

void phi2_init(void)
{
    phi2_set_khz_run(phi2_get_khz());
}

bool phi2_check_khz(uint16_t *v)
{
    return *v >= PHI2_MIN_KHZ && *v <= PHI2_MAX_KHZ;
}

void phi2_apply_khz(uint16_t phi2_khz, bool changed)
{
    (void)changed;
    phi2_set_khz_run(phi2_khz);
}
