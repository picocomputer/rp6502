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

/* The beam line count this last ran up to. The beam is the machine's clock,
 * so run time is a function of the frames that went by and not of the host's
 * clock, which is what lets a timed test repeat. */
static uint64_t bus_lines;

/* What is left of the cycle budget, in sixty-thirds of a cycle. Signed because
 * the budget rounds up, so a scanline can overshoot into the next one's debt. */
static int64_t bus_owed;

static uint64_t bus_cycle_count;

/* The bus as it stands between run_until calls. The data byte and the two
 * interrupt lines carry across cycles because the CPU latches the settled data
 * on the next tick and samples the interrupt line there too. IRQB is wired-OR
 * on silicon, but each device keeps its own line here so that no device has to
 * clear another's; bus_tick ORs them at the CPU. */
static uint16_t bus_addr;
static uint8_t bus_data;
static bool bus_read;
static bool bus_via_irq;
static bool bus_ria_irq;

uint64_t bus_cycles(void) { return bus_cycle_count; }

void bus_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put_u64(c, bus_lines);
    sst_put_i64(c, bus_owed);
    sst_put_u64(c, bus_cycle_count);
    sst_put_u16(c, bus_addr);
    sst_put_u8(c, bus_data);
    sst_put_bool(c, bus_read);
    sst_put_bool(c, bus_via_irq);
    sst_put_bool(c, bus_ria_irq);
}

bool bus_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    uint64_t lines = sst_get_u64(c);
    int64_t owed = sst_get_i64(c);
    uint64_t count = sst_get_u64(c);
    uint16_t addr = sst_get_u16(c);
    uint8_t data = sst_get_u8(c);
    bool read = sst_get_bool(c);
    bool via_irq = sst_get_bool(c);
    bool ria_irq = sst_get_bool(c);
    if (!sst_ok(c))
        return false;
    bus_lines = lines;
    bus_owed = owed;
    bus_cycle_count = count;
    bus_addr = addr;
    bus_data = data;
    bus_read = read;
    bus_via_irq = via_irq;
    bus_ria_irq = ria_irq;
    return true;
}

void bus_reset(void)
{
    bus_addr = 0;
    bus_data = 0;
    bus_read = true;
    bus_via_irq = false;
    bus_ria_irq = false;
}

static inline void bus_hoist(uint16_t *addr, uint8_t *data, bool *read,
                             bool *via_irq, bool *ria_irq)
{
    *addr = bus_addr;
    *data = bus_data;
    *read = bus_read;
    *via_irq = bus_via_irq;
    *ria_irq = bus_ria_irq;
}

static inline void bus_park(uint16_t addr, uint8_t data, bool read,
                            bool via_irq, bool ria_irq)
{
    bus_addr = addr;
    bus_data = data;
    bus_read = read;
    bus_via_irq = via_irq;
    bus_ria_irq = ria_irq;
}

/* One PHI2 cycle of everything on the bus. The CPU is the only bus master, so
 * it must tick first and drive the address before the devices answer. Only
 * $0000-$FEFF, $FFD0-$FFDF and $FFE0-$FFFF answer a read, so no two devices
 * drive data in the same cycle. */
static inline void bus_tick(uint16_t *addr, uint8_t *data, bool *read,
                            bool *via_irq, bool *ria_irq)
{
    cpu_tick(addr, read, data, *via_irq || *ria_irq);
    *via_irq = via_tick(*addr, *read, data);
    *ria_irq = ria_tick(*addr, *read, data);
    sram_tick(*addr, *read, data);
}

/* Run the cycles the scanlines since last time were worth. The budget is spent
 * whether or not those cycles run, so a machine held in reset or stopped at a
 * breakpoint drops what is left rather than banking it; banking it would make a
 * resumed machine run a burst proportional to how long it was held. */
static void run_until(uint64_t lines)
{
    /* A scanline is worth 2*khz/63 cycles, because the beam runs 525 scanlines
     * at 60 Hz and khz*1000 cycles a second over 31500 scanlines a second
     * reduces to 2/63. Accumulating in sixty-thirds keeps that exact. The
     * division rounds up so that the cycle straddling the boundary runs on
     * this scanline. */
    bus_owed += (int64_t)(lines - bus_lines) * phi2_get_khz_run() * 2;
    bus_lines = lines;
    int64_t n = (bus_owed + 62) / 63;
    bus_owed -= n * 63;
    if (n <= 0)
        return;

    /* Nothing else reads the bus mid-scanline, so the loop can own it in
     * locals and park it on the way out. */
    uint16_t addr;
    uint8_t data;
    bool read;
    bool via_irq;
    bool ria_irq;
    bus_hoist(&addr, &data, &read, &via_irq, &ria_irq);
    uint64_t ran = 0;
    if (!dbg_is_active())
    {
        /* Two loops rather than one with a per-cycle test, because PHI2 runs
         * at up to 8 MHz and the debug branch is worth keeping out of the
         * common path. */
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
            /* The same condition sram_tick uses: every write lands in sram[],
             * whatever the address, but only $0000-$FEFF answers a read. */
            if (dbg_watch_armed && (!read || addr <= SRAM_MMAP_HI))
                dbg_watch_access(addr, data, !read);
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
