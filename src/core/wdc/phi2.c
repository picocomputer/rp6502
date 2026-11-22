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

static bool phi2_sst_valid(sst_cursor_t *c)
{
    if (!phi2_check_khz(&khz_run))
        return sst_refuse(c, "a PHI2 rate out of range");
    return true;
}

/* A savestate carries the rate the machine is running at, not the rate it is
 * configured for: a ROM may set its own, and a reset takes it back to the
 * configured one. */
#define PHI2_SST(X)                                             \
    X(U16, khz_run)                                             \
    X(VALIDATE, phi2_sst_valid)
SST_TABLE(phi2, PHI2_SST)

void phi2_set_khz_run(uint16_t khz)
{
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
