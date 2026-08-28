/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "core/api/proc_exec.h"
#include "core/aud/aud_mix.h"
#include "core/dap/dbg.h"
#include "core/sys/log.h"
#include "core/rom/rom.h"
#include "core/hid/vtkeys.h"
#include "core/mach.h"
#include "drivers.h"
#include "core/wdc/cpu.h"
#include "core/mem/mem.h"
#include "core/ria/ria.h"
#include "core/sys/sys.h"
#include "core/vga/vga_emu.h"
#include "core/wdc/via.h"
#include "core/api/api.h"
#include "core/api/std.h"
#include "core/str/rln.h"
#include "core/term/term.h"
#include "host.h"
#include <stdio.h>

/* The system clock, oversampled — see SYS_OVERSAMPLE. Wraps in centuries. */
static uint64_t sys_clk;

/* The bus between run_until calls, which hoists it into locals for the loop. data and
 * the IRQs carry across cycles: the CPU latches the settled data on the next tick, and
 * samples the interrupt line there too. IRQB is wired-OR, but each device keeps its
 * own line so none has to clear another's — sys_tick ORs them at the CPU. */
static uint16_t bus_addr;
static uint8_t bus_data;
static bool bus_read;
static bool bus_via_irq;
static bool bus_ria_irq;

uint64_t host_clock_us(void) { return sys_clk / SYS_TICKS_PER_US; }

/* No init: mach_init runs exactly once per process, so static zero-initialization
 * is the cold-boot state. (sys_init in ria/sys/sys.h is the firmware's monitor
 * banner, which the emulator does not implement.) */

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

/* Park it back. Paired with sys_clk at every return from run_until — miss one and a
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

/* One PHI2 cycle of the whole machine — the board wiring, in the floooh/chips
 * system-tick style (see _vic20_tick). The CPU is the only bus master and drives the
 * bus in decoded signals; every device ticks each cycle (the VIA counts its timers,
 * the RIA drives IRQB and publishes its pins) and decodes its own window, so the
 * board holds no chip-select state. The read ranges do not overlap, so the order
 * here does not matter.
 *
 * The bus arrives by pointer because run_until owns it as locals for the duration of
 * the loop, not as the file statics it is parked in between calls. */
static inline void sys_tick(uint16_t *addr, uint8_t *data, bool *read,
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
     * sys->pins). Measured break-even here — the statics were a single cache line the
     * store buffer forwarded — so this is for the intent, not a win. */
    uint64_t clk = sys_clk;
    uint16_t addr;
    uint8_t data;
    bool read;
    bool via_irq;
    bool ria_irq;
    bus_hoist(&addr, &data, &read, &via_irq, &ria_irq);
    const uint32_t cycle_ticks = cpu_cycle_ticks();
    if (!dbg_is_active())
    {
        /* Two loops rather than a per-cycle test, per vic20_exec: at ~8M cycles a
         * second the debug branch is worth keeping out of the common path. */
        while (clk < deadline && cpu_active())
        {
            sys_tick(&addr, &data, &read, &via_irq, &ria_irq);
            clk += cycle_ticks;
        }
    }
    else
    {
        while (clk < deadline && cpu_active() && !dbg_is_stopped())
        {
            sys_tick(&addr, &data, &read, &via_irq, &ria_irq);
            clk += cycle_ticks;
            if (cpu_dbg_cycle_cb)
                cpu_dbg_cycle_cb(cpu_dbg_pins());
            /* Data breakpoints. Only the accesses mem_tick serviced count, so reads a
             * device drove are excluded — watchpoints cover the SRAM, not registers. */
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
    sys_clk = clk;
    bus_park(addr, data, read, via_irq, ria_irq);
}

/* This driver's task: run the 6502 up to wherever video got to. Bounded by
 * construction -- vga_task advances at most one scanline per pass, so this is
 * at most one scanline of cycles. */
void cpu_task(void)
{
    run_until(vga_beam_clk());
}

/* The console's task on a machine whose console is the terminal the walk
 * already reaches. The consoles with a transport of their own -- a UART, a
 * fabric bridge -- do real work here; see core/com/com.h. */
void com_task(void) {}

/* One pass of this machine's super-loop: both task columns of the drivers
 * its drivers.h lists, then whatever run or stop was asked for along the way.
 * The firmware mains are this loop with a different list. */
void main_task(void)
{
#define DRIVER(i, t, iot, r, s, b) t();
    MACH_FORWARD(RP6502_MACH_DRIVERS)
#undef DRIVER
#define DRIVER(i, t, iot, r, s, b) iot();
    MACH_FORWARD(RP6502_MACH_DRIVERS)
#undef DRIVER
    mach_commit();
}

/* Nowhere to break to: none of the software machines has a monitor, so the
 * key that asked is an ordinary key. */
bool mach_break(void)
{
    return false;
}

bool mach_break_to_launcher(void)
{
    return false;
}
