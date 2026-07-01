/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Top level: owns the 65C02 core and the one master clock. Every chips tick
 * advances the 256 MHz master clock by the PHI2 divider; the VGA scanlines and
 * the s/ds/cs/ms timers are deadline consumers of that same clock, so the whole
 * machine is paced by one reproducible time base.
 */

#include "emu/api/api.h"
#include "emu/api/oem.h"
#include "emu/api/pro.h"
#include "emu/api/std.h"
#include "emu/aud/snd.h"
#include "emu/dbg/dbg.h"
#include "emu/mon/rom.h"
#include "emu/sys/mem.h"
#include "emu/chips/rp6502.h"
#include "emu/sys/sys.h"
#include "emu/sys/vga.h"
#include "emu/sys/via.h"
#include "emu/chips/w65c02.h"
#include "aud/aud.h"
#include "str/rln.h"
#include "sys/cpu.h"
#include <stdio.h>
#include <string.h>

static m6502_t cpu;
static uint64_t pins;

/* Display-only per-cycle observer for the on-screen ui_dbg view (declared in
 * sys.h). The window overlay registers dbgui_tick here; NULL otherwise, so the
 * hot tick loop pays only a null check. It MUST NOT gate the CPU — dbg.c is the
 * one authoritative engine. */
void (*emu_dbg_cycle_cb)(uint64_t pins);

/* Pending exec (op 0x09): the new program loads at the frame boundary rather
 * than mid-tick, so the master clock and the partially-run frame stay
 * consistent. emu_exec() captures the ROM path (resolved by emu_rom_load) and
 * stops the current program. */
static bool exec_pending;
static char exec_path[FS_HOST_MAX_PATH];

void emu_exec(const char *rom_path)
{
    snprintf(exec_path, sizeof(exec_path), "%s", rom_path);
    exec_pending = true;
    emu_cpu_halted = true; /* stop the current program; the tick loop exits */
}

/* The live 65C02 instance, for the debugger UI + DAP register access. */
void *sys_cpu(void) { return &cpu; }

/* Total VGA frames run since start (diagnostic: should advance at 60 Hz). */
unsigned long emu_vga_frame_count;

/* ------------------------------------------------------------------ */
/* Master clock                                                        */
/* ------------------------------------------------------------------ */

/* The master clock in 1/8-of-a-256MHz-tick units. Held that fine so the PHI2
 * fractional divider lands on an integer per-cycle step. Wraps in centuries. */
static uint64_t master_8;

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

uint64_t emu_now_us(void)
{
    /* 256 MHz -> 256 ticks/us -> 2048 eighth-ticks/us. */
    return master_8 / 2048;
}

/* ------------------------------------------------------------------ */
/* PHI2 (the 6502 clock), a fractional divider of the master clock     */
/* ------------------------------------------------------------------ */

static uint16_t phi2_khz_run;   /* achievable PHI2 after quantization (reported) */
static uint32_t master_per_cycle_8 = 256; /* 1/8-ticks advanced per 6502 cycle */

/* Mirror ria/sys/cpu.c cpu_change_phi2_khz: the 6502:RP2350 ratio is 1:32, so
 * clkdiv = (256MHz/32)/phi2 = 8000/phi2 as int + 8-bit frac. The master clock
 * then advances 32*clkdiv ticks per cycle = (256*clkdiv_int + clkdiv_frac)/8. */
void emu_set_phi2_khz(uint16_t khz)
{
    if (khz < EMU_PHI2_MIN_KHZ)
        khz = EMU_PHI2_MIN_KHZ;
    if (khz > EMU_PHI2_MAX_KHZ)
        khz = EMU_PHI2_MAX_KHZ;
    float clkdiv = (EMU_RP2350_KHZ / 32.0f) / khz;
    uint16_t clkdiv_int = (uint16_t)clkdiv;
    uint8_t clkdiv_frac = (uint8_t)((clkdiv - clkdiv_int) * 256.0f);
    phi2_khz_run = (uint16_t)((EMU_RP2350_KHZ / 32.0f) / (clkdiv_int + clkdiv_frac / 256.0f));
    master_per_cycle_8 = 256u * clkdiv_int + clkdiv_frac;
}

uint16_t emu_get_phi2_khz(void)
{
    return phi2_khz_run;
}

/* sys/cpu.h names the vendored firmware (atr.c) reaches PHI2 through; there is
 * no separate config vs. run clock in the emulator, so both map straight here. */
void cpu_set_phi2_khz_run(uint16_t khz) { emu_set_phi2_khz(khz); }
uint16_t cpu_get_phi2_khz_run(void) { return emu_get_phi2_khz(); }
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

void emu_init(void)
{
    exec_pending = false;
    master_8 = 0; /* run time starts at boot */
    scanline_n = 0;
    emu_vga_frame_count = 0;
    emu_set_phi2_khz(EMU_PHI2_DEFAULT_KHZ); /* --phi2 reapplies after emu_init */
    aud_init();                             /* fill the shared sine table (no PWM off-device) */
    ria_reset();
    oem_reset();        /* cold boot: default code page 437 (exec preserves the page) */
    vga_boot_console(); /* font_init loads that same 437 default into the font */
    /* m6502_init returns a pin mask with RES asserted; the first ticks run
     * the reset sequence and fetch the vector at $FFFC/$FFFD. */
    pins = m6502_init(&cpu, &(m6502_desc_t){0});
    via_reset(); /* the VIA shares the 6502's RESB, so a CPU reset clears it */
}

/* Advance one 60 Hz VGA frame (525 scanlines). Within each scanline the 6502 is
 * pumped until the master clock reaches that scanline's deadline — so the CPU
 * runs PHI2/scanline-rate cycles and the video is paced by the same clock. The
 * vsync counter ($FFE3) ticks at the highest scanline any program renders. The
 * app loop calls this at 60 Hz regardless of the host display's refresh rate. */
static void run_frame(bool render)
{
    /* Debugger hold: a stopped CPU freezes virtual time and the window simply
     * re-presents the last captured frame. Hoisted once per frame so the hot
     * tick loop pays nothing when debugging is inactive (the common case). */
    const bool dbg = dbg_is_active();
    if (dbg && dbg_is_stopped())
        return;

    const int vsync_line = vga_vsync_scanline();
    const int canvas_h = vga_canvas_height(); /* visible region; snapshot for the frame */
    const uint64_t frame_end_n = scanline_n + EMU_VGA_SCANLINES;
    int line = 0; /* 0-based scanline within this frame */
    bool vsynced = false;
    bool dbg_break = false;

    while (scanline_n < frame_end_n)
    {
        /* Raster-accurate scanout: draw this visible line from the CURRENT
         * machine state BEFORE its CPU cycles run, so a mid-frame register/VRAM
         * write only affects later lines (real per-scanline VGA behavior). A
         * catch-up frame (render == false) skips the pixels but keeps the timing. */
        if (render && line < canvas_h)
            vga_render_scanline(line);

        const uint64_t deadline = scanline_deadline_8(scanline_n + 1);
        while (master_8 < deadline && !emu_cpu_halted)
        {
            pins = m6502_tick(&cpu, pins);
            pins = via_tick(pins); /* counts the VIA timers + drives M6502_IRQ */
            pins = ria_tick(pins); /* RIA window access + additive $FFF0 IRQ (after the VIA) */
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
                {
                    dbg_break = true;
                    break;
                }
            }
        }
        if (dbg_break)
            break;
        if (master_8 < deadline)
            master_8 = deadline; /* halted: keep the master clock (time) flowing */
        std_task();      /* drain read_xram's PIX gate before the op re-polls */
        ria_task_pump(); /* poll in-flight I/O each scanline (RIA super-loop analog) */
        scanline_n++;
        if (!vsynced && line + 1 >= vsync_line)
        {
            REGS(0xFFE3) = (uint8_t)(REGS(0xFFE3) + 1); /* VSYNC counter, 8-bit wrap */
            ria_trigger_vsync(); /* latch $FFF0 bit7; raises IRQ only if the program enabled it */
            vsynced = true;
            /* The visible lines were rendered as the beam passed them (above), so
             * g_present already holds the completed frame at this point. */
        }
        line++;
    }

    if (dbg_break)
        return; /* held at a breakpoint mid-frame; resume re-enters emu_run_frame */

    emu_vga_frame_count++;
    /* Pump the line editor (drains keyboard + terminal replies, echoes, fires
     * the read callback) then advance any blocking syscall waiting on it. */
    rln_task();
    ria_task();
    vga_task(); /* cursor/cell blink, evaluated at the new virtual time */
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
            pins = m6502_init(&cpu, &(m6502_desc_t){0});
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

/* Hand back the frame captured at the last vsync boundary (what the window
 * presents), so --screenshot and the tests see the same completed frame. */
void emu_render(uint32_t *fb)
{
    int w, h;
    emu_canvas_size(&w, &h);
    memcpy(fb, emu_present_framebuffer(), (size_t)w * h * sizeof(uint32_t));
}
