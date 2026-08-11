/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mmio.h"
#include "ria/sys/cpu.h"

static uint16_t cpu_phi2_khz_set = CPU_PHI2_DEFAULT;

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
    cpu_phi2_khz_set = phi2_khz;
    MMIO_PHI2 = phi2_khz;
    return true;
}

uint16_t cpu_get_phi2_khz(void)
{
    return cpu_phi2_khz_set;
}

void cpu_init(void)
{
    CPU_RESB = 0;
    MMIO_PHI2 = cpu_phi2_khz_set;
}

void cpu_run(void)
{
    REGS_WIN[0x10] = 0;
    CPU_RESB = 1;
}

void cpu_stop(void)
{
    CPU_RESB = 0;
    MMIO_PHI2 = cpu_phi2_khz_set;
}

bool cpu_active(void)
{
    return CPU_RESB != 0;
}
