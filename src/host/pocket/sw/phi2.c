/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The phase accumulator in phi2.sv produces exactly the rate held in
 * MMIO_PHI2, averaged over many periods, at every whole kilohertz, so
 * phi2_get_khz_run returns the register unchanged.
 */

#include "mmio.h"
#include "core/wdc/phi2.h"
#include "core/sys/config.h"

void phi2_set_khz_run(uint16_t phi2_khz)
{
    if (phi2_khz < PHI2_MIN_KHZ)
        phi2_khz = PHI2_MIN_KHZ;
    if (phi2_khz > PHI2_MAX_KHZ)
        phi2_khz = PHI2_MAX_KHZ;
    MMIO_PHI2 = phi2_khz;
}

uint16_t phi2_get_khz_run(void)
{
    return (uint16_t)MMIO_PHI2;
}

bool phi2_check_khz(uint16_t *v)
{
    return *v >= PHI2_MIN_KHZ && *v <= PHI2_MAX_KHZ;
}

void phi2_apply_khz(uint16_t phi2_khz, bool changed)
{
    (void)changed;
    MMIO_PHI2 = phi2_khz;
}

void phi2_init(void)
{
    MMIO_PHI2 = phi2_get_khz();
}
