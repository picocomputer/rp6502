/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * There is no clock divider on a software machine. The beam is the machine's
 * clock and the bus converts scanlines into cycles with exact integer
 * arithmetic, so every whole kilohertz in range is exact and phi2_check_khz
 * can accept all of them.
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

void phi2_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put_u16(c, khz_run);
}

bool phi2_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    uint16_t khz = sst_get_u16(c);
    if (!sst_ok(c) || khz < PHI2_MIN_KHZ || khz > PHI2_MAX_KHZ)
        return false;
    khz_run = khz;
    return true;
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
