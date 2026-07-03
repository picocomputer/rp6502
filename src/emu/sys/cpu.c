/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/chips/rp6502.h"
#include "emu/chips/w65c02.h"
#include "emu/dbg/dbg.h"
#include "emu/sys/cpu.h"
#include "emu/sys/mem.h"
#include "emu/sys/sys.h"
#include "emu/sys/via.h"

static m6502_t cpu;
static uint64_t pins;

/* Display-only per-cycle observer for the on-screen ui_dbg view (declared in
 * cpu.h). The window overlay registers dbgui_tick here; NULL otherwise, so the
 * hot tick loop pays only a null check. It MUST NOT gate the CPU — dbg.c is the
 * one authoritative engine. */
void (*emu_dbg_cycle_cb)(uint64_t pins);

/* The live 65C02 instance, for the debugger UI + DAP register access. */
void *cpu_chip(void) { return &cpu; }

/* The master clock in 1/8-of-a-256MHz-tick units. Held that fine so the PHI2
 * fractional divider lands on an integer per-cycle step. Wraps in centuries. */
static uint64_t master_8;

uint64_t emu_now_us(void)
{
    /* 256 MHz -> 256 ticks/us -> 2048 eighth-ticks/us. */
    return master_8 / 2048;
}

/* ------------------------------------------------------------------ */
/* PHI2 (the 6502 clock), a fractional divider of the master clock     */
/* ------------------------------------------------------------------ */

static uint16_t phi2_khz_run;             /* achievable PHI2 after quantization (reported) */
static uint32_t master_per_cycle_8 = 256; /* 1/8-ticks advanced per 6502 cycle */

/* Mirror ria/sys/cpu.c cpu_change_phi2_khz: the 6502:RP2350 ratio is 1:32, so
 * clkdiv = (256MHz/32)/phi2 = 8000/phi2 as int + 8-bit frac. The master clock
 * then advances 32*clkdiv ticks per cycle = (256*clkdiv_int + clkdiv_frac)/8. */
void cpu_set_phi2_khz_run(uint16_t khz)
{
    if (khz < CPU_PHI2_MIN_KHZ)
        khz = CPU_PHI2_MIN_KHZ;
    if (khz > CPU_PHI2_MAX_KHZ)
        khz = CPU_PHI2_MAX_KHZ;
    float clkdiv = (CPU_RP2350_KHZ / 32.0f) / khz;
    uint16_t clkdiv_int = (uint16_t)clkdiv;
    uint8_t clkdiv_frac = (uint8_t)((clkdiv - clkdiv_int) * 256.0f);
    phi2_khz_run = (uint16_t)((CPU_RP2350_KHZ / 32.0f) / (clkdiv_int + clkdiv_frac / 256.0f));
    master_per_cycle_8 = 256u * clkdiv_int + clkdiv_frac;
}

uint16_t cpu_get_phi2_khz_run(void)
{
    return phi2_khz_run;
}

bool cpu_active(void) { return !emu_cpu_halted; }

static inline uint64_t bus_cycle(uint64_t p)
{
    uint16_t addr = M6502_GET_ADDR(p);
    /* The RIA ($FFE0-$FFF9) and VIA ($FFD0-$FFDF) windows are already serviced on
     * the pins by ria_tick / via_tick (data is on the pins for a read; a write was
     * consumed) — leave them alone here, this branch only backs RAM. */
    if ((addr >= RIA_WINDOW_LO && addr <= RIA_WINDOW_HI) ||
        (addr >= VIA_WINDOW_LO && addr <= VIA_WINDOW_HI))
        return p;
    if (p & M6502_RW)
    {
        M6502_SET_DATA(p, ram[addr]);
    }
    else
        ram[addr] = M6502_GET_DATA(p);
    return p;
}

void cpu_init(void)
{
    master_8 = 0; /* run time starts at boot */
    cpu_set_phi2_khz_run(CPU_PHI2_DEFAULT);
}

/* m6502_init returns a pin mask with RES asserted; the first ticks run
 * the reset sequence and fetch the vector at $FFFC/$FFFD. */
void cpu_reset(void)
{
    pins = m6502_init(&cpu, &(m6502_desc_t){0});
}

bool cpu_run_until(uint64_t deadline_8, bool dbg)
{
    while (master_8 < deadline_8 && !emu_cpu_halted)
    {
        pins = m6502_tick(&cpu, pins);
        pins = via_tick(pins);  /* counts the VIA timers + drives M6502_IRQ */
        pins = ria_tick(pins);  /* RIA window access + additive $FFF0 IRQ (after the VIA) */
        pins = bus_cycle(pins); /* RAM (the peripheral windows were serviced above) */
        master_8 += master_per_cycle_8;
        if (dbg)
        {
            /* Feed the on-screen overlay's ui_dbg view every cycle (its
             * disassembly heatmap/history/PC); display-only, never gates the
             * CPU. NULL unless the window overlay registered it. */
            if (emu_dbg_cycle_cb)
                emu_dbg_cycle_cb(pins);
            /* Breakpoint/step check at each opcode fetch (M6502_SYNC). Stops
             * before the instruction's effect runs; the partial frame is then
             * abandoned and the machine holds until the debugger resumes. */
            if ((pins & M6502_SYNC) &&
                dbg_at_instruction(M6502_GET_ADDR(pins), m6502_s(&cpu)))
                return true; /* clock deliberately NOT clamped: frame abandoned */
        }
    }
    if (master_8 < deadline_8)
        master_8 = deadline_8; /* halted: keep the master clock (time) flowing */
    return false;
}
