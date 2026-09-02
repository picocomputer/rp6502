/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/wdc/bus.h"
#include "core/dap/dbg.h"
#include "core/mem.h"
#include "core/mem/mem.h"
#include "core/ria/ria.h"
#include "core/wdc/phi2_div.h"
#include "core/wdc/resb.h"
#include "core/wdc/via.h"
#include "core/wdc/w65c02.h"
#include "core/vga/vga_emu.h"
#include "host/host.h"

/* The system clock, oversampled -- see SYS_OVERSAMPLE. Wraps in centuries.
 * Nothing else advances it: the CPU catching up to the beam is the only thing
 * that does, against an absolute per-scanline deadline and never the host's
 * clock. So run time is a reproducible function of the frames that went by,
 * which is what makes a timed test repeat. */
static uint64_t bus_clk;

/* The divider's phase, carried cycle to cycle so every whole kilohertz is
 * exact. Parked here with the clock it advances. */
static uint32_t bus_phase;

/* The bus between run_until calls, which hoists it into locals for the loop.
 * data and the IRQs carry across cycles: the CPU latches the settled data on
 * the next tick, and samples the interrupt line there too. IRQB is wired-OR,
 * but each device keeps its own line so none has to clear another's --
 * bus_tick ORs them at the CPU. */
static uint16_t bus_addr;
static uint8_t bus_data;
static bool bus_read;
static bool bus_via_irq;
static bool bus_ria_irq;

uint64_t host_clock_us(void) { return bus_clk / SYS_TICKS_PER_US; }

void bus_reset(void)
{
    bus_addr = 0;
    bus_data = 0;
    bus_read = true;
    bus_via_irq = false;
    bus_ria_irq = false;
}

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

/* Park it back. Paired with bus_clk at every return from run_until -- miss one
 * and a resumed frame drives a stale bus. */
static inline void bus_park(uint16_t addr, uint8_t data, bool read,
                            bool via_irq, bool ria_irq)
{
    bus_addr = addr;
    bus_data = data;
    bus_read = read;
    bus_via_irq = via_irq;
    bus_ria_irq = ria_irq;
}

/* One PHI2 cycle of everything on the bus, in the floooh/chips system-tick
 * style (see _vic20_tick). The CPU is the only bus master and drives the bus
 * in decoded signals; every device ticks each cycle (the VIA counts its
 * timers, the RIA drives IRQB and publishes its pins) and decodes its own
 * window, so nothing holds chip-select state. The read ranges do not overlap,
 * so the order here does not matter.
 *
 * The bus arrives by pointer because run_until owns it as locals for the
 * duration of the loop, not as the file statics it is parked in between
 * calls. */
static inline void bus_tick(uint16_t *addr, uint8_t *data, bool *read,
                            bool *via_irq, bool *ria_irq)
{
    cpu_tick(addr, read, data, *via_irq || *ria_irq);
    *via_irq = via_tick(*addr, *read, data);
    *ria_irq = ria_tick(*addr, *read, data);
    mem_tick(*addr, *read, data);
}

/* Run 6502 cycles until the system clock reaches deadline. The clock is at
 * deadline or later on return, always: a machine held in reset and a
 * debugger-held one both stop fetching, and time goes on without them -- lost
 * cycles, as RDY held on silicon. */
static void run_until(uint64_t deadline)
{
    /* Hoist the clock and the bus into locals and commit both before every
     * return: nothing else reads either mid-scanline, so the loop never
     * touches the statics and the compiler is free to keep the bus in
     * registers (as vic20_exec does with sys->pins). Measured break-even here
     * -- the statics were a single cache line the store buffer forwarded --
     * so this is for the intent, not a win. */
    uint64_t clk = bus_clk;
    uint16_t addr;
    uint8_t data;
    bool read;
    bool via_irq;
    bool ria_irq;
    bus_hoist(&addr, &data, &read, &via_irq, &ria_irq);
    const phi2_div_t divider = phi2_div();
    /* A rate change between calls can leave a phase the new rate never
     * reaches; it owes at most one tick, so drop it rather than drain it. */
    uint32_t phase = bus_phase < divider.khz ? bus_phase : 0;
    if (!dbg_is_active())
    {
        /* Two loops rather than a per-cycle test, per vic20_exec: at ~8M
         * cycles a second the debug branch is worth keeping out of the common
         * path. */
        while (clk < deadline && resb_running())
        {
            bus_tick(&addr, &data, &read, &via_irq, &ria_irq);
            clk += divider.whole;
            phase += divider.frac;
            if (phase >= divider.khz)
            {
                phase -= divider.khz;
                clk++;
            }
        }
    }
    else
    {
        while (clk < deadline && resb_running() && !dbg_is_stopped())
        {
            bus_tick(&addr, &data, &read, &via_irq, &ria_irq);
            clk += divider.whole;
            phase += divider.frac;
            if (phase >= divider.khz)
            {
                phase -= divider.khz;
                clk++;
            }
            if (cpu_dbg_cycle_cb)
                cpu_dbg_cycle_cb(cpu_dbg_pins());
            /* Data breakpoints. Only the accesses mem_tick serviced count, so
             * reads a device drove are excluded -- watchpoints cover the SRAM,
             * not registers. */
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
    bus_clk = clk;
    bus_phase = phase;
    bus_park(addr, data, read, via_irq, ria_irq);
}

void bus_task(void)
{
    run_until(vga_beam_clk());
}
