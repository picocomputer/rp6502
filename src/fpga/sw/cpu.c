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

/* The system setting, which a ROM's own choice of clock only borrows.
 * The register holds what is running; this holds what to go back to. */
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

/* RESB low before anything else, the way the RP2350's cpu_init drives
 * it. Both registers here are the OS's alone — no reset reaches either —
 * so this line is what holds the 6502 and what decides the clock. Without
 * it a host reset comes back with the last program still running at
 * whatever speed it asked for. */
void cpu_init(void)
{
    CPU_RESB = 0;
    MMIO_PHI2 = cpu_phi2_khz_set;
}

/* The RIA's own cells are the firmware's to establish, the way
 * ria_run() does it on the RP2350: the 6502 is released against a state
 * somebody chose rather than whatever the cells happened to hold. The
 * fabric used to cover for this with an asynchronous reset across every
 * register in the machine, which is not the contract — it is a reset
 * fan-out standing in for an init nobody wrote.
 *
 * $FFF0 is the API's own register and the one ria_run() clears by name.
 * The vectors are written by the loader before this, because only it
 * knows where the image starts. */
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

/* The machine's lifecycle contract, minimally. */
bool cpu_active(void)
{
    return CPU_RESB != 0;
}
