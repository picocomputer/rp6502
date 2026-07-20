/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/api/pro.h"
#include "emu/aud/aud.h"
#include "emu/dbg/dbg.h"
#include "emu/host/rom.h"
#include "emu/main.h"
#include "emu/sys/cpu.h"
#include "emu/sys/mem.h"
#include "emu/sys/ria.h"
#include "emu/sys/sys.h"
#include "emu/sys/vga.h"
#include "emu/sys/via.h"
#include "ria/api/api.h"
#include "ria/api/std.h"
#include "ria/str/rln.h"
#include "vga/term/term.h"
#include <stdio.h>

/* The system clock, oversampled — see SYS_OVERSAMPLE. Wraps in centuries. */
static uint64_t sys_clk;

/* Absolute, never reset per frame — feeds the exact deadline math below. */
static uint64_t scanline_n;

static unsigned long frame_count;

/* The bus between run_until calls, which hoists it into locals for the loop. data and
 * the IRQs carry across cycles: the CPU latches the settled data on the next tick, and
 * samples the interrupt line there too. IRQB is wired-OR, but each device keeps its
 * own line so none has to clear another's — sys_tick ORs them at the CPU. */
static uint16_t bus_addr;
static uint8_t bus_data;
static bool bus_read;
static bool bus_via_irq;
static bool bus_ria_irq;

uint64_t sys_clk_now(void) { return sys_clk; }
unsigned long sys_frame_count(void) { return frame_count; }

/* No init: main_init runs exactly once per process, so static zero-initialization
 * is the cold-boot state. (sys_init in ria/sys/sys.h is the firmware's monitor
 * banner, which the emulator does not implement.) */

/* Deadline at which scanline n is due:
 *   n * (SYS_RP2350_KHZ*1000 * SYS_OVERSAMPLE) / (60*525 scanline/s)
 *     = n * 4096000 / 63  (reduced).
 * Computed from the ABSOLUTE scanline number every time — never accumulated — so the
 * integer division introduces NO drift: it is exact at every frame boundary (n a
 * multiple of 525, since 31500/63 = 500). Do NOT "fix" the non-exact 4096000/63 by
 * tracking a per-scanline remainder; that would double-correct and create real drift.
 * The n*4096000 intermediate overflows uint64 ~4.5 years of uptime (well before
 * sys_clk itself), still unreachable. */
static uint64_t scanline_deadline(uint64_t n)
{
    return n * 4096000ull / 63;
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

/* Run 6502 cycles until the system clock reaches deadline, the program halts, or
 * (dbg) an instruction breakpoint stops the machine. Returns true on a breakpoint
 * stop, leaving the clock mid-scanline; otherwise the clock is at deadline or later
 * on return (time flows even while halted). */
static bool run_until(uint64_t deadline, bool dbg)
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
    if (!dbg)
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
        while (clk < deadline && cpu_active())
        {
            sys_tick(&addr, &data, &read, &via_irq, &ria_irq);
            clk += cycle_ticks;
            if (cpu_dbg_cycle_cb)
                cpu_dbg_cycle_cb(cpu_dbg_pins());
            /* Data breakpoints. Only the accesses mem_tick serviced count, so reads a
             * device drove are excluded — watchpoints cover the SRAM, not registers. */
            if (dbg_watch_armed && (!read || addr <= MEM_MMAP_HI))
                dbg_watch_access(addr, data, !read);
            /* Stop before the fetched instruction's effect runs; the partial frame
             * is then abandoned and the machine holds until resume. */
            uint16_t pc;
            uint8_t sp;
            if (cpu_opcode_fetch(&pc, &sp) && dbg_at_instruction(pc, sp))
            {
                sys_clk = clk; /* commit both before abandoning the frame */
                bus_park(addr, data, read, via_irq, ria_irq);
                return true;
            }
        }
    }
    if (clk < deadline)
        clk = deadline; /* halted: keep the clock (time) flowing */
    sys_clk = clk;
    bus_park(addr, data, read, via_irq, ria_irq);
    return false;
}

/* Advance one 60 Hz VGA frame (525 scanlines). Within each scanline the 6502 is
 * pumped until the system clock reaches that scanline's deadline — so the CPU runs
 * PHI2/scanline-rate cycles and the video is paced by the same clock. The vsync
 * counter ($FFE3) ticks at the highest scanline any program renders. The app loop
 * calls this at 60 Hz regardless of the host display's refresh rate. */
static void run_frame(bool render)
{
    /* Debugger hold: only the 6502 and virtual time freeze. Console output that
     * reached the terminal after the beam passed its row this frame (a program's
     * final prints before the stop) hasn't been scanned out yet, so sweep the
     * visible canvas once from the frozen state; after that the window simply
     * re-presents the settled frame. Hoisted once per frame so the hot tick loop
     * pays nothing when debugging is inactive (the common case). */
    static bool stop_swept;
    const bool dbg = dbg_is_active();
    if (dbg && dbg_is_stopped())
    {
        if (render && !stop_swept)
        {
            const int h = vga_canvas_height();
            for (int line = 0; line < h; line++)
                vga_render_scanline(line);
            stop_swept = true;
        }
        return;
    }
    stop_swept = false;

    vga_task(); /* perform an armed console reset before rendering this frame */

    const int vsync_line = vga_vsync_scanline();
    const int canvas_h = vga_canvas_height();
    const uint64_t frame_end_n = scanline_n + VGA_SCANLINES;
    int line = 0; /* 0-based scanline within this frame */
    bool vsynced = false;

    while (scanline_n < frame_end_n)
    {
        /* Raster-accurate scanout: draw this visible line from the CURRENT machine
         * state BEFORE its CPU cycles run, so a mid-frame register/VRAM write only
         * affects later lines (real per-scanline VGA behavior). A catch-up frame
         * (render == false) skips the pixels but keeps the timing. */
        if (render && line < canvas_h)
            vga_render_scanline(line);

        if (run_until(scanline_deadline(scanline_n + 1), dbg))
            return; /* held at a breakpoint mid-frame; resume re-runs the frame */
        std_task(); /* drain read_xram's PIX gate before the op re-polls */
        api_task(); /* poll in-flight I/O each scanline (RIA super-loop analog) */
        term_task(); /* VGA chip super-loop analog: per scanline, so the
                      * one-row-per-tick lazy clears drain within the frame
                      * that issued them, not one row per frame */
        scanline_n++;
        if (!vsynced && line + 1 >= vsync_line)
        {
            REGS(0xFFE3) = (uint8_t)(REGS(0xFFE3) + 1); /* VSYNC counter, 8-bit wrap */
            ria_trigger_vsync(); /* latch $FFF0 bit7; raises IRQ only if the program enabled it */
            vsynced = true;
        }
        line++;
    }

    frame_count++;
    /* Pump the line editor (drains keyboard + terminal replies, echoes, fires the
     * read callback) then advance any blocking syscall waiting on it. */
    rln_task();
    ria_task();
    aud_task();

    /* An exec committed this frame: load the new program and restart the CPU,
     * keeping the system clock and the argv pro_api_exec stored. main_stop arms the
     * console reset (vga_task performs it before the new program draws), as on real
     * hardware; the screen text survives (preserve-screen terminal RIS). */
    const char *exec_path = pro_take_exec();
    if (exec_path)
    {
        main_stop(); /* tear down the outgoing program (cpu_stop halts it) */
        if (!rom_load(exec_path))
        {
            fprintf(stderr, "rp6502-emu: exec failed to load '%s'\n", exec_path);
            main_set_exit_code(1); /* stays halted from main_stop */
        }
        else
            main_run(); /* start the incoming program; keeps VSYNC + clock */
    }
}

void sys_run_frame(void) { run_frame(true); }

/* Run one frame WITHOUT rendering — a catch-up frame the pacer will not present.
 * CPU/chip/timing/vsync all advance; only the per-scanline pixel work is skipped
 * (most of the per-frame cost), so catching up after a slow/stalled host is cheap. */
void sys_run_frame_norender(void) { run_frame(false); }
