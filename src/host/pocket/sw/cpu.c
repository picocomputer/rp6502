/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * PHI2, which on this machine is one register. The accumulator in
 * phi2_div is exact at every whole kilohertz, so what was asked for is
 * what runs and the run value is read back rather than shadowed.
 *
 * No configuration store behind the setting, and nothing off-machine
 * sets it, so set and load differ only in who clamped first.
 */

#include "mmio.h"
#include "cpu.h"

/* The register holds what is running; this holds what to go back to. */
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

/* No reset reaches either register, so this is what holds the 6502 and
 * what decides the clock after a host reset. */
void cpu_init(void)
{
    CPU_RESB = 0;
    MMIO_PHI2 = cpu_phi2_khz_set;
}

/* The 6502 is released against a state the firmware chose, not whatever
 * the cells held. The vectors were written by the loader before this. */
void cpu_run(void)
{
    REGS_WIN[0x10] = 0; /* $FFF0 */
    CPU_RESB = 1;
}

/* A ROM that changed the clock does not get to leave it changed. */
void cpu_stop(void)
{
    CPU_RESB = 0;
    MMIO_PHI2 = cpu_phi2_khz_set;
}

bool cpu_active(void)
{
    return CPU_RESB != 0;
}
