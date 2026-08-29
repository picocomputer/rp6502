/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "chips/chips/w65c02.h"
#include "core/wdc/cpu.h"
#include "core/dap/dbg.h"
#include "core/mem.h"
#include "core/mem/mem.h"
#include "core/ria/ria.h"
#include "core/rp2350.h"
#include "core/vga/vga_emu.h"
#include "core/wdc/via.h"
#include "host.h"

static w65c02_t cpu;

/* The 6502 bus, in the w65c02's own pin layout. Private to this file — the board
 * speaks decoded signals. w65c02_init seeds it with RES asserted. */
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

/* Program start: w65c02_init returns a pin mask with RES asserted; the first ticks
 * run the reset sequence and fetch the vector at $FFFC/$FFFD. Must be last in the
 * run fan-out (the VIA shares RESB, so via_run runs just before). */
void cpu_run(void)
{
    pins = w65c02_init(&cpu, &(w65c02_desc_t){0});
    halted = false;
}

/* Program stop: freeze the 6502 (the tick loop runs only while cpu_active()). */
void cpu_stop(void)
{
    halted = true;
}

/* One PHI2 cycle. The 6502 pin mask is the w65c02's own layout, so it never leaves
 * this file: the board hands back the settled bus as decoded signals and gets the
 * next cycle's drive the same way. */
void cpu_tick(uint16_t *addr, bool *read, uint8_t *data, bool irq)
{
    if (irq)
        pins |= W65C02_IRQ;
    else
        pins &= ~W65C02_IRQ;
    W65C02_SET_DATA(pins, *data);

    pins = w65c02_tick(&cpu, pins);

    *addr = W65C02_GET_ADDR(pins);
    *read = (pins & W65C02_RW) != 0;
    *data = W65C02_GET_DATA(pins);
}

uint32_t cpu_cycle_ticks(void) { return cycle_ticks; }

/* The raw pin mask, for the debugger's per-cycle observer only — its callback
 * contract is the w65c02 layout. The non-debug loop never calls this. */
uint64_t cpu_dbg_pins(void) { return pins; }

bool cpu_opcode_fetch(uint16_t *pc, uint8_t *sp)
{
    if (!(pins & W65C02_SYNC))
        return false;
    *pc = W65C02_GET_ADDR(pins);
    *sp = w65c02_s(&cpu);
    return true;
}

/* ------------------------------------------------------------------ */
/* The bus this chip masters, and the clock it counts                  */
/* ------------------------------------------------------------------ */

/* The system clock, oversampled -- see SYS_OVERSAMPLE. Wraps in centuries.
 * Nothing else advances it: the CPU catching up to the beam is the only thing
 * that does, against an absolute per-scanline deadline and never the host's
 * clock. So run time is a reproducible function of the frames that went by,
 * which is what makes a timed test repeat. */
static uint64_t cpu_clk;

/* The bus between run_until calls, which hoists it into locals for the loop. data and
 * the IRQs carry across cycles: the CPU latches the settled data on the next tick, and
 * samples the interrupt line there too. IRQB is wired-OR, but each device keeps its
 * own line so none has to clear another's -- bus_tick ORs them at the CPU. */
static uint16_t bus_addr;
static uint8_t bus_data;
static bool bus_read;
static bool bus_via_irq;
static bool bus_ria_irq;

uint64_t host_clock_us(void) { return cpu_clk / SYS_TICKS_PER_US; }

/* No init: sys_init runs exactly once per process, so static zero-initialization
 * is the cold-boot state. */

/* Take the parked bus for the run_until loop to own as locals. */
static inline void bus_hoist(uint16_t *addr, uint8_t *data, bool *read,
                             bool *via_irq, bool *ria_irq)
{
    *addr = bus_addr;
    *data = bus_data;
    *read = bus_read;
    *via_irq = bus_via_irq;
    *ria_irq = bus_ria_irq;
}

/* Park it back. Paired with cpu_clk at every return from run_until -- miss one and a
 * resumed frame drives a stale bus. */
static inline void bus_park(uint16_t addr, uint8_t data, bool read,
                            bool via_irq, bool ria_irq)
{
    bus_addr = addr;
    bus_data = data;
    bus_read = read;
    bus_via_irq = via_irq;
    bus_ria_irq = ria_irq;
}

/* One PHI2 cycle of everything on the bus, in the floooh/chips system-tick style (see
 * _vic20_tick). The CPU is the only bus master and drives the bus in decoded signals;
 * every device ticks each cycle (the VIA counts its timers, the RIA drives IRQB and
 * publishes its pins) and decodes its own window, so nothing holds chip-select state.
 * The read ranges do not overlap, so the order here does not matter.
 *
 * The bus arrives by pointer because run_until owns it as locals for the duration of
 * the loop, not as the file statics it is parked in between calls. */
static inline void bus_tick(uint16_t *addr, uint8_t *data, bool *read,
                            bool *via_irq, bool *ria_irq)
{
    cpu_tick(addr, read, data, *via_irq || *ria_irq);
    *via_irq = via_tick(*addr, *read, data);
    *ria_irq = ria_tick(*addr, *read, data);
    mem_tick(*addr, *read, data);
}

/* Run 6502 cycles until the system clock reaches deadline. The clock is at
 * deadline or later on return, always: a halted 6502 and a debugger-held one
 * both stop fetching, and time goes on without them -- lost cycles, as RDY
 * held on silicon. */
static void run_until(uint64_t deadline)
{
    /* Hoist the clock and the bus into locals and commit both before every return:
     * nothing else reads either mid-scanline, so the loop never touches the statics
     * and the compiler is free to keep the bus in registers (as vic20_exec does with
     * sys->pins). Measured break-even here -- the statics were a single cache line the
     * store buffer forwarded -- so this is for the intent, not a win. */
    uint64_t clk = cpu_clk;
    uint16_t addr;
    uint8_t data;
    bool read;
    bool via_irq;
    bool ria_irq;
    bus_hoist(&addr, &data, &read, &via_irq, &ria_irq);
    const uint32_t tick_ticks = cpu_cycle_ticks();
    if (!dbg_is_active())
    {
        /* Two loops rather than a per-cycle test, per vic20_exec: at ~8M cycles a
         * second the debug branch is worth keeping out of the common path. */
        while (clk < deadline && cpu_active())
        {
            bus_tick(&addr, &data, &read, &via_irq, &ria_irq);
            clk += tick_ticks;
        }
    }
    else
    {
        while (clk < deadline && cpu_active() && !dbg_is_stopped())
        {
            bus_tick(&addr, &data, &read, &via_irq, &ria_irq);
            clk += tick_ticks;
            if (cpu_dbg_cycle_cb)
                cpu_dbg_cycle_cb(cpu_dbg_pins());
            /* Data breakpoints. Only the accesses mem_tick serviced count, so reads a
             * device drove are excluded -- watchpoints cover the SRAM, not registers. */
            if (dbg_watch_armed && (!read || addr <= MEM_MMAP_HI))
                dbg_watch_access(addr, data, !read);
            /* Stop before the fetched instruction's effect runs. The loop
             * ends; the clock still reaches the deadline below. */
            uint16_t pc;
            uint8_t sp;
            if (cpu_opcode_fetch(&pc, &sp))
                dbg_at_instruction(pc, sp);
        }
    }
    if (clk < deadline)
        clk = deadline; /* nobody fetching: time flows anyway */
    cpu_clk = clk;
    bus_park(addr, data, read, via_irq, ria_irq);
}

/* This driver's task: run the 6502 up to wherever video got to. Bounded by
 * construction -- vga_task advances at most one scanline per pass, so this is
 * at most one scanline of cycles. */
void cpu_task(void)
{
    run_until(vga_beam_clk());
}
