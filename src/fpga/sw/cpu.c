/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * PHI2, which on this machine is one register. The accumulator in
 * phi2_div is exact at every whole kilohertz, so there is nothing to
 * round-trip and nothing to divide: what was asked for is what runs, and
 * the run value can simply be read back rather than shadowed. The RIA
 * keeps a copy because its PIO divider quantizes and it has to report
 * what it actually got.
 *
 * There is no configuration file behind the setting either. The Pocket's
 * own menu holds it, so set and load are the same act and the only
 * difference left between the two verbs is who clamped first.
 */

#include "mmio.h"
#include "ria/sys/cpu.h"

void cpu_set_phi2_khz_run(uint16_t phi2_khz)
{
    if (phi2_khz < CPU_PHI2_MIN_KHZ)
        phi2_khz = CPU_PHI2_MIN_KHZ;
    if (phi2_khz > CPU_PHI2_MAX_KHZ)
        phi2_khz = CPU_PHI2_MAX_KHZ;
    MMIO_PHI2 = phi2_khz;
}

uint16_t cpu_get_phi2_khz_run(void)
{
    return (uint16_t)MMIO_PHI2;
}

bool cpu_set_phi2_khz(uint16_t phi2_khz)
{
    if (phi2_khz < CPU_PHI2_MIN_KHZ || phi2_khz > CPU_PHI2_MAX_KHZ)
        return false;
    MMIO_PHI2 = phi2_khz;
    return true;
}

uint16_t cpu_get_phi2_khz(void)
{
    return (uint16_t)MMIO_PHI2;
}

/* The machine's lifecycle contract, minimally. */
bool cpu_active(void)
{
    return CPU_RUN != 0;
}
