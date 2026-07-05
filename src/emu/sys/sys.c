/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/host/msc.h"
#include "emu/api/oem.h"
#include "emu/api/pro.h"
#include "emu/api/std.h"
#include "emu/aud/snd.h"
#include "emu/dbg/dbg.h"
#include "emu/mon/rom.h"
#include "emu/sys/com.h"
#include "emu/sys/cpu.h"
#include "emu/sys/mem.h"
#include "emu/chips/rp6502.h"
#include "emu/sys/sys.h"
#include "emu/sys/vga.h"
#include "emu/sys/via.h"
#include "api/api.h"
#include "aud/aud.h"
#include "str/rln.h"
#include "term/term.h"
#include <stdio.h>

/* Pending exec (op 0x09): the new program loads at the frame boundary rather
 * than mid-tick, so the master clock and the partially-run frame stay
 * consistent. emu_exec() captures the ROM path (resolved by emu_rom_load) and
 * stops the current program. */
static bool exec_pending;
static char exec_path[HOST_MSC_MAX_PATH];

void emu_exec(const char *rom_path)
{
    snprintf(exec_path, sizeof(exec_path), "%s", rom_path);
    exec_pending = true;
    emu_cpu_halted = true; /* stop the current program; the tick loop exits */
}

/* Total VGA frames run since start (diagnostic: should advance at 60 Hz). */
unsigned long emu_vga_frame_count;

/* VGA scanline deadlines and counters, all on the master clock. */
static uint64_t scanline_n; /* total scanlines emitted since boot */

/* Deadline (1/8-tick units) at which scanline n is due:
 *   n * (8 * 256e6 sub/s) / (60*525 scanline/s) = n * 4096000 / 63  (reduced).
 * Computed from the ABSOLUTE scanline number every time — never accumulated —
 * so the integer division introduces NO drift: it is exact at every frame
 * boundary (n a multiple of 525, since 31500/63 = 500). Do NOT "fix" the
 * non-exact 4096000/63 by tracking a per-scanline remainder; that would
 * double-correct and create real drift. The n*4096000 intermediate overflows
 * uint64 ~4.5 years of uptime (well before master_8 itself), still unreachable. */
static inline uint64_t scanline_deadline_8(uint64_t n)
{
    return n * 4096000ull / 63;
}

void emu_init(void)
{
    exec_pending = false;
    cpu_init(); /* master clock to boot, default PHI2 (--phi2 reapplies after emu_init) */
    scanline_n = 0;
    emu_vga_frame_count = 0;
    aud_init(); /* fill the shared sine table (no PWM off-device) */
    ria_reset();
    com_reset();        /* cold boot: flush queued input (per-exec keeps type-ahead) */
    oem_reset();        /* cold boot: default code page 437 (exec preserves the page) */
    vga_boot_console(); /* font_init loads that same 437 default into the font */
    cpu_reset();
    via_reset(); /* the VIA shares the 6502's RESB, so a CPU reset clears it */
}

/* Advance one 60 Hz VGA frame (525 scanlines). Within each scanline the 6502 is
 * pumped until the master clock reaches that scanline's deadline — so the CPU
 * runs PHI2/scanline-rate cycles and the video is paced by the same clock. The
 * vsync counter ($FFE3) ticks at the highest scanline any program renders. The
 * app loop calls this at 60 Hz regardless of the host display's refresh rate. */
static void run_frame(bool render)
{
    /* Debugger hold: only the 6502 and virtual time freeze. Console output that
     * reached the terminal after the beam passed its row this frame (a program's
     * final prints before the stop) hasn't been scanned out yet, so sweep the
     * visible canvas once from the frozen state; after that the window simply
     * re-presents the settled frame. Hoisted once per frame so the hot tick
     * loop pays nothing when debugging is inactive (the common case). */
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

    const int vsync_line = vga_vsync_scanline();
    const int canvas_h = vga_canvas_height(); /* visible region; snapshot for the frame */
    const uint64_t frame_end_n = scanline_n + EMU_VGA_SCANLINES;
    int line = 0; /* 0-based scanline within this frame */
    bool vsynced = false;

    while (scanline_n < frame_end_n)
    {
        /* Raster-accurate scanout: draw this visible line from the CURRENT
         * machine state BEFORE its CPU cycles run, so a mid-frame register/VRAM
         * write only affects later lines (real per-scanline VGA behavior). A
         * catch-up frame (render == false) skips the pixels but keeps the timing. */
        if (render && line < canvas_h)
            vga_render_scanline(line);

        if (cpu_run_until(scanline_deadline_8(scanline_n + 1), dbg))
            return; /* held at a breakpoint mid-frame; resume re-enters emu_run_frame */
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
            /* The visible lines were rendered as the beam passed them (above), so
             * the app's framebuffer already holds the completed frame here. */
        }
        line++;
    }

    emu_vga_frame_count++;
    /* Pump the line editor (drains keyboard + terminal replies, echoes, fires
     * the read callback) then advance any blocking syscall waiting on it. */
    rln_task();
    ria_task();
    snd_task(); /* generate this frame's audio from the active RIA device */

    /* An exec committed this frame: load the new program and restart the CPU,
     * keeping the master clock and the argv pro_api_exec stored. The terminal
     * and VGA state are NOT reset — the new program's output appends to the
     * existing screen, as on real hardware. */
    if (exec_pending)
    {
        exec_pending = false;
        if (!emu_rom_load(exec_path))
        {
            fprintf(stderr, "rp6502-emu: exec failed to load '%s'\n", exec_path);
            emu_cpu_halted = true;
            emu_exit_code = 1;
        }
        else
        {
            ria_reset(); /* RIA/std/kbd/atr/clk; clears halt, keeps VSYNC + screen */
            cpu_reset();
            via_reset();
            pro_run(); /* the reloaded program (exec or launcher) is now running */
        }
    }
}

/* Run one frame and render it (the displayed frame). */
void emu_run_frame(void) { run_frame(true); }

/* Run one frame WITHOUT rendering — a catch-up frame the pacer will not present.
 * CPU/chip/timing/vsync all advance; only the per-scanline pixel work is skipped
 * (most of the per-frame cost), so catching up after a slow/stalled host is cheap. */
void emu_run_frame_norender(void) { run_frame(false); }
