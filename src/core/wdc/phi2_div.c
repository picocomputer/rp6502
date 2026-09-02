/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/wdc/phi2_div.h"
#include "core/wdc/bus.h"
#include "core/sys/config.h"

/* Ticks per millisecond, against a rate in cycles per millisecond. */
#define PHI2_TICKS_PER_MS (SYS_TICKS_PER_US * 1000u)

static phi2_div_t div = {
    PHI2_TICKS_PER_MS / PHI2_DEFAULT_KHZ,
    PHI2_TICKS_PER_MS % PHI2_DEFAULT_KHZ,
    PHI2_DEFAULT_KHZ,
};

void phi2_set_khz_run(uint16_t khz)
{
    if (khz < PHI2_MIN_KHZ)
        khz = PHI2_MIN_KHZ;
    if (khz > PHI2_MAX_KHZ)
        khz = PHI2_MAX_KHZ;
    div.whole = PHI2_TICKS_PER_MS / khz;
    div.frac = PHI2_TICKS_PER_MS % khz;
    div.khz = khz;
}

uint16_t phi2_get_khz_run(void)
{
    return (uint16_t)div.khz;
}

phi2_div_t phi2_div(void)
{
    return div;
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
