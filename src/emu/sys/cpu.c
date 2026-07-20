/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#define CHIPS_IMPL
#include "emu/chips/w65c02.h"
#include "emu/sys/cpu.h"
#include "ria/sys/sys.h"

static m6502_t cpu;

/* The 6502 bus, in the m6502's own pin layout. Private to this file — the board
 * speaks decoded signals. m6502_init seeds it with RES asserted. */
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
static uint32_t cycle_ticks = 256; /* system-clock ticks per 6502 cycle */

/* Mirror ria/sys/cpu.c cpu_change_phi2_khz: the 6502:RP2350 ratio is 1:32, so
 * clkdiv = (256MHz/32)/phi2 = 8000/phi2 as int + 8-bit frac. The system clock is
 * counted oversampled (see SYS_OVERSAMPLE), so a cycle is 256*int + frac of it —
 * an exact integer, which is the whole point of the oversample. */
void cpu_set_phi2_khz_run(uint16_t khz)
{
    if (khz < CPU_PHI2_MIN_KHZ)
        khz = CPU_PHI2_MIN_KHZ;
    if (khz > CPU_PHI2_MAX_KHZ)
        khz = CPU_PHI2_MAX_KHZ;
    float clkdiv = (SYS_RP2350_KHZ / 32.0f) / khz;
    uint16_t clkdiv_int = (uint16_t)clkdiv;
    uint8_t clkdiv_frac = (uint8_t)((clkdiv - clkdiv_int) * 256.0f);
    phi2_khz_run = (uint16_t)((SYS_RP2350_KHZ / 32.0f) / (clkdiv_int + clkdiv_frac / 256.0f));
    cycle_ticks = 256u * clkdiv_int + clkdiv_frac;
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

bool cpu_active(void) { return !halted; }
bool cpu_halted(void) { return halted; }
void cpu_set_halted(bool on) { halted = on; }

void cpu_init(void)
{
    cpu_set_phi2_khz_run(phi2_khz_cfg ? phi2_khz_cfg : CPU_PHI2_DEFAULT);
}

/* Program start: m6502_init returns a pin mask with RES asserted; the first ticks
 * run the reset sequence and fetch the vector at $FFFC/$FFFD. Must be last in the
 * run fan-out (the VIA shares RESB, so via_run runs just before). */
void cpu_run(void)
{
    pins = m6502_init(&cpu, &(m6502_desc_t){0});
    halted = false;
}

/* Program stop: freeze the 6502 (the tick loop runs only while cpu_active()). */
void cpu_stop(void)
{
    halted = true;
}

/* One PHI2 cycle. The 6502 pin mask is the m6502's own layout, so it never leaves
 * this file: the board hands back the settled bus as decoded signals and gets the
 * next cycle's drive the same way. */
void cpu_tick(bool irq, uint16_t *addr, bool *read, uint8_t *data)
{
    if (irq)
        pins |= M6502_IRQ;
    else
        pins &= ~M6502_IRQ;
    M6502_SET_DATA(pins, *data);

    pins = m6502_tick(&cpu, pins);

    *addr = M6502_GET_ADDR(pins);
    *read = (pins & M6502_RW) != 0;
    *data = M6502_GET_DATA(pins);
}

uint32_t cpu_cycle_ticks(void) { return cycle_ticks; }

/* The raw pin mask, for the debugger's per-cycle observer only — its callback
 * contract is the m6502 layout. The non-debug loop never calls this. */
uint64_t cpu_dbg_pins(void) { return pins; }

bool cpu_opcode_fetch(uint16_t *pc, uint8_t *sp)
{
    if (!(pins & M6502_SYNC))
        return false;
    *pc = M6502_GET_ADDR(pins);
    *sp = m6502_s(&cpu);
    return true;
}
