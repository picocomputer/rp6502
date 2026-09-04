/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/wdc/bus.h"
#include "core/dap/dbg.h"
#include "core/wdc/sram.h"
#include "core/ria/ria.h"
#include "core/wdc/phi2.h"
#include "core/wdc/resb.h"
#include "core/wdc/via.h"
#include "core/wdc/cpu.h"
#include "core/vga/vga_emu.h"

/* Scanlines this has already answered for. The beam is the machine's clock
 * and runs whether or not the CPU does, so the only question here is how many
 * cycles the lines since last time were worth. Run time is therefore a
 * function of the frames that went by and not of the host's clock, which is
 * what makes a timed test repeat. */
static uint64_t bus_lines;

/* The cycle budget's remainder, in sixty-thirds of a cycle. Signed because
 * the budget rounds up -- the cycle that crosses a line boundary is run on
 * that line, as it always was, and the overshoot is the next line's debt. */
static int64_t bus_owed;

/* Cycles run, for the test that pins how many a frame is worth. */
static uint64_t bus_cycle_count;

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

uint64_t bus_cycles(void) { return bus_cycle_count; }

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
    sram_tick(*addr, *read, data);
}

/* Run the cycles the scanlines since last time were worth. A machine held in
 * reset and a debugger-held one both stop fetching, and the beam goes on
 * without them: whatever is left of the budget is dropped rather than banked,
 * or a resumed machine would run a burst proportional to how long it was held.
 * Lost cycles, as RDY held on silicon. */
static void run_until(uint64_t lines)
{
    /* A scanline is 2*khz/63 cycles. Accumulate in sixty-thirds and round up,
     * because the cycle that crosses the boundary is run on this line -- which
     * is what the old tick loop did by testing clk < deadline. */
    bus_owed += (int64_t)(lines - bus_lines) * phi2_get_khz_run() * 2;
    bus_lines = lines;
    int64_t n = (bus_owed + 62) / 63;
    bus_owed -= n * 63;
    if (n <= 0)
        return;

    /* Hoist the bus into locals and commit before every return: nothing else
     * reads it mid-scanline, so the loop never touches the statics and the
     * compiler is free to keep the bus in registers (as vic20_exec does with
     * sys->pins). Measured break-even here -- the statics were a single cache
     * line the store buffer forwarded -- so this is for the intent, not a win. */
    uint16_t addr;
    uint8_t data;
    bool read;
    bool via_irq;
    bool ria_irq;
    bus_hoist(&addr, &data, &read, &via_irq, &ria_irq);
    uint64_t ran = 0;
    if (!dbg_is_active())
    {
        /* Two loops rather than a per-cycle test, per vic20_exec: at ~8M
         * cycles a second the debug branch is worth keeping out of the common
         * path. */
        while (ran < (uint64_t)n && resb_running())
        {
            bus_tick(&addr, &data, &read, &via_irq, &ria_irq);
            ++ran;
        }
    }
    else
    {
        while (ran < (uint64_t)n && resb_running() && !dbg_is_stopped())
        {
            bus_tick(&addr, &data, &read, &via_irq, &ria_irq);
            ++ran;
            if (cpu_dbg_cycle_cb)
                cpu_dbg_cycle_cb(cpu_dbg_pins());
            /* Data breakpoints. Only the accesses sram_tick serviced count, so
             * reads a device drove are excluded -- watchpoints cover the SRAM,
             * not registers. */
            if (dbg_watch_armed && (!read || addr <= SRAM_MMAP_HI))
                dbg_watch_access(addr, data, !read);
            /* Stop before the fetched instruction's effect runs. The loop
             * ends; the beam goes on without it. */
            uint16_t pc;
            uint8_t sp;
            if (cpu_opcode_fetch(&pc, &sp))
                dbg_at_instruction(pc, sp);
        }
    }
    bus_cycle_count += ran;
    bus_park(addr, data, read, via_irq, ria_irq);
}

void bus_task(void)
{
    run_until(vga_beam_lines());
}
