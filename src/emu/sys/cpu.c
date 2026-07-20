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
#include "emu/sys/via.h"
#include "pico/time.h"

static m6502_t cpu;
static uint64_t pins;

/* Display-only per-cycle observer for the on-screen ui_dbg view (declared in
 * cpu.h). The window overlay registers dbgui_tick here; NULL otherwise, so the
 * hot tick loop pays only a null check. It MUST NOT gate the CPU — dbg.c is the
 * one authoritative engine. */
void (*cpu_dbg_cycle_cb)(uint64_t pins);

/* The live 65C02 instance, for the debugger UI + DAP register access. */
void *cpu_chip(void) { return &cpu; }

/* ------------------------------------------------------------------ */
/* PHI2 (the 6502 clock), a fractional divider of the master clock     */
/* ------------------------------------------------------------------ */

static uint16_t phi2_khz_run;             /* achievable PHI2 after quantization (reported) */
static uint16_t phi2_khz_cfg;             /* config PHI2 loaded before init (0 = built-in default) */
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

/* Config PHI2 — the machine default, loaded before cpu_init (firmware cfg_init
 * parity). Validated here; cpu_init quantizes it into the run clock. */
bool cpu_set_phi2_khz(uint16_t khz)
{
    if (khz < CPU_PHI2_MIN_KHZ || khz > CPU_PHI2_MAX_KHZ)
        return false;
    phi2_khz_cfg = khz;
    return true;
}

/* Program-halt gate: set true by the EXIT syscall, a failed exec, or a --dap
 * launch hold; cleared by cpu_run on (re)start. */
static bool halted;
static uint64_t cpu_start_us;

bool cpu_active(void) { return !halted; }
bool cpu_halted(void) { return halted; }
void cpu_set_halted(bool on) { halted = on; }

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
        if (__builtin_expect(dbg_watch_armed, 0))
            dbg_watch_access(addr, ram[addr], false);
    }
    else
    {
        ram[addr] = M6502_GET_DATA(p);
        if (__builtin_expect(dbg_watch_armed, 0))
            dbg_watch_access(addr, ram[addr], true);
    }
    return p;
}

void cpu_init(void)
{
    cpu_set_phi2_khz_run(phi2_khz_cfg ? phi2_khz_cfg : CPU_PHI2_DEFAULT);
}

/* Program start: m6502_init returns a pin mask with RES asserted; the first ticks
 * run the reset sequence and fetch the vector at $FFFC/$FFFD. Must be last in the
 * run fan-out (the VIA shares RESB, so via_run runs just before). */
void cpu_run(void)
{
    cpu_start_us = time_us_64();
    pins = m6502_init(&cpu, &(m6502_desc_t){0});
    halted = false;
}

/* From the one master clock, so run time is a reproducible function of the frame
 * count (the vendored atr.c reads it for the s/ds/cs/ms attributes). */
uint32_t cpu_get_run(uint32_t us_per_tick)
{
    return (uint32_t)((time_us_64() - cpu_start_us) / us_per_tick);
}

/* Program stop: freeze the 6502 (the tick loop runs only while cpu_active()). */
void cpu_stop(void)
{
    halted = true;
}

uint64_t cpu_tick(void)
{
    pins = m6502_tick(&cpu, pins);
    pins = via_tick(pins);  /* counts the VIA timers + drives M6502_IRQ */
    pins = ria_tick(pins);  /* RIA window access + additive $FFF0 IRQ (after the VIA) */
    pins = bus_cycle(pins); /* RAM (the peripheral windows were serviced above) */
    return pins;
}

uint32_t cpu_step_8(void) { return master_per_cycle_8; }

bool cpu_opcode_fetch(uint64_t pins, uint16_t *pc, uint8_t *sp)
{
    if (!(pins & M6502_SYNC))
        return false;
    *pc = M6502_GET_ADDR(pins);
    *sp = m6502_s(&cpu);
    return true;
}
