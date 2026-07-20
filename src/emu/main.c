/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/main.h"
#include "emu/api/pro.h"
#include "emu/aud/aud.h"
#include "emu/dbg/dbg.h"
#include "emu/host/msc.h"
#include "emu/host/rom.h"
#include "emu/host/tmp.h"
#include "emu/sys/com.h"
#include "emu/sys/cpu.h"
#include "emu/sys/mem.h"
#include "emu/sys/pix.h"
#include "emu/sys/vga.h"
#include "emu/sys/via.h"
#include "emu/hid/kbd.h"
#include "emu/hid/mou.h"
#include "emu/hid/pad.h"
#include "emu/hid/tab.h"
#include "emu/chips/rp6502.h"
#include "ria/api/api.h"
#include "ria/api/atr.h"
#include "ria/api/std.h"
#include "ria/api/fat.h"
#include "ria/api/clk.h"
#include "ria/api/oem.h"
#include "ria/aud/aud.h"
#include "ria/aud/psg.h"
#include "ria/aud/opl.h"
#include "ria/str/rln.h"
#include "ria/str/str.h"
#include "ria/sys/sys.h"
#include "vga/term/font.h"
#include "vga/term/term.h"
#include "vga/modes/mode1.h"
#include "vga/modes/mode2.h"
#include "vga/modes/mode3.h"
#include "vga/modes/mode4.h"
#include "vga/modes/mode5.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Machine-global run state                                            */
/* ------------------------------------------------------------------ */

/* The virtual master clock, in 1/8-of-a-256 MHz-tick units (2048/µs) so the PHI2
 * fractional divider lands on an integer per-cycle step. Wraps in centuries. */
static uint64_t master_8;

static int s_exit_code;
static unsigned long s_frame_count;

/* Absolute, never reset per frame — feeds the exact deadline math below. */
static uint64_t scanline_n;

int main_exit_code(void) { return s_exit_code; }
void main_set_exit_code(int code) { s_exit_code = code; }
unsigned long main_frame_count(void) { return s_frame_count; }
uint64_t main_clock_8(void) { return master_8; }

/* Power-up initialization — called exactly ONCE per process (never re-run; init
 * is not idempotent). Settings are loaded before this (cpu/oem adopt them), the
 * ROM is loaded after, and the caller starts the machine with main_run(). */
void main_init(void)
{
    pro_init();
    cpu_init();
    master_8 = 0;
    scanline_n = 0;
    s_frame_count = 0;
    aud_init();
    com_init();
    std_init();
    rln_init();
    sys_init();
    str_init();
    oem_init();
    font_init();
    term_init();
    vga_init();
}

/* ------------------------------------------------------------------ */
/* Machine lifecycle: start/stop the 6502 (mirrors ria/main.c run/stop) */
/* ------------------------------------------------------------------ */

/* Start running the 6502 — the counterpart to ria/main.c's run(), same order
 * (cpu_run last; via_run beside it, the VIA shares the 6502 RESB). Unlike the
 * firmware's main_run (a scheduler request), this fans out immediately: the emu is
 * frame-driven and has no run-state machine. The code page and PHI2 are deliberately
 * NOT reset — an exec'd program inherits the parent's (an intentional divergence
 * from hardware; the program reads the page back via the CODE_PAGE attribute). The
 * cold-boot defaults live in main_init and cpu_init. */
void main_run(void)
{
    s_exit_code = 0;
    xstack[XSTACK_SIZE] = 0; /* cstring guard */
    pro_run();
    com_run();
    rln_run();
    fat_run();
    api_run();
    sys_run();
    ria_run();
    via_run();
    cpu_run(); /* must be last */
}

/* Stop the 6502 — the counterpart to ria/main.c's stop(), same order (cpu_stop
 * first, vga_stop second). The emu omits the modules it has no analog for
 * (pix/mid/mdm/rom/mon) and, deliberately, oem_stop: the code page rings through. */
void main_stop(void)
{
    cpu_stop(); /* must be first */
    vga_stop(); /* arm the console reset (firmware stop() resets the VGA) */
    rln_stop();
    api_stop();
    std_stop();
    fat_stop();
    msc_stop();
    kbd_stop();
    mou_stop();
    pad_stop();
    tab_stop();
    aud_stop();
}

/* ------------------------------------------------------------------ */
/* Run engine                                                          */
/* ------------------------------------------------------------------ */

/* Deadline (1/8-tick units) at which scanline n is due:
 *   n * (8 * 256e6 sub/s) / (60*525 scanline/s) = n * 4096000 / 63  (reduced).
 * Computed from the ABSOLUTE scanline number every time — never accumulated —
 * so the integer division introduces NO drift: it is exact at every frame
 * boundary (n a multiple of 525, since 31500/63 = 500). Do NOT "fix" the
 * non-exact 4096000/63 by tracking a per-scanline remainder; that would
 * double-correct and create real drift. The n*4096000 intermediate overflows
 * uint64 ~4.5 years of uptime (well before master_8 itself), still unreachable. */
static uint64_t scanline_deadline_8(uint64_t n)
{
    return n * 4096000ull / 63;
}

/* Run 6502 cycles until the master clock reaches deadline_8, the program halts,
 * or (dbg) an instruction breakpoint stops the machine. Returns true on a
 * breakpoint stop, leaving the clock mid-scanline; otherwise the clock is at
 * deadline_8 or later on return (time flows even while halted). */
static bool run_until(uint64_t deadline_8, bool dbg)
{
    /* Accumulate in a local and commit to master_8 before every return: nothing
     * else reads the clock mid-scanline, so this keeps the hot loop off the static. */
    uint64_t clock_8 = master_8;
    const uint32_t step_8 = cpu_step_8();
    while (clock_8 < deadline_8 && cpu_active())
    {
        uint64_t pins = cpu_tick();
        clock_8 += step_8;
        if (dbg)
        {
            if (cpu_dbg_cycle_cb)
                cpu_dbg_cycle_cb(pins);
            /* Stop before the fetched instruction's effect runs; the partial
             * frame is then abandoned and the machine holds until resume. */
            uint16_t pc;
            uint8_t sp;
            if (cpu_opcode_fetch(pins, &pc, &sp) && dbg_at_instruction(pc, sp))
            {
                master_8 = clock_8; /* commit before abandoning the frame */
                return true;
            }
        }
    }
    if (clock_8 < deadline_8)
        clock_8 = deadline_8; /* halted: keep the master clock (time) flowing */
    master_8 = clock_8;
    return false;
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

    vga_task(); /* perform an armed console reset before rendering this frame */

    const int vsync_line = vga_vsync_scanline();
    const int canvas_h = vga_canvas_height();
    const uint64_t frame_end_n = scanline_n + VGA_SCANLINES;
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

        if (run_until(scanline_deadline_8(scanline_n + 1), dbg))
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

    s_frame_count++;
    /* Pump the line editor (drains keyboard + terminal replies, echoes, fires
     * the read callback) then advance any blocking syscall waiting on it. */
    rln_task();
    ria_task();
    aud_task();

    /* An exec committed this frame: load the new program and restart the CPU,
     * keeping the master clock and the argv pro_api_exec stored. main_stop arms
     * the console reset (vga_task performs it before the new program draws), as on
     * real hardware; the screen text survives (preserve-screen terminal RIS). */
    const char *exec_path = pro_take_exec();
    if (exec_path)
    {
        main_stop(); /* tear down the outgoing program (cpu_stop halts it) */
        if (!rom_load(exec_path))
        {
            fprintf(stderr, "rp6502-emu: exec failed to load '%s'\n", exec_path);
            s_exit_code = 1; /* stays halted from main_stop */
        }
        else
            main_run(); /* start the incoming program; keeps VSYNC + clock */
    }
}

void main_run_frame(void) { run_frame(true); }

/* Run one frame WITHOUT rendering — a catch-up frame the pacer will not present.
 * CPU/chip/timing/vsync all advance; only the per-scanline pixel work is skipped
 * (most of the per-frame cost), so catching up after a slow/stalled host is cheap. */
void main_run_frame_norender(void) { run_frame(false); }

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

/* PIX XREG register dispatch. Device 0 is the RIA-local virtual device (HID +
 * audio); device 1 is the VGA. False on an unhandled channel/address. */
bool main_xreg_0(uint8_t channel, uint8_t address, uint16_t word)
{
    if (channel == 0) /* human interface devices -> XRAM report blocks */
    {
        if (address == 0)
            return kbd_set_xram(word);
        if (address == 1)
            return mou_set_xram(word);
        if (address == 2)
            return pad_set_xram(word);
        if (address == 3)
            return tab_set_xram(word);
        return false;
    }
    if (channel == 1) /* audio: PSG at address 0, OPL at address 1 */
    {
        if (address == 0)
            return psg_xreg(word);
        if (address == 1)
            return opl_xreg(word);
        return false;
    }
    return false;
}

/* The VGA mode-xreg accumulator, shared by channel 0 (CANVAS/MODE) and channel 15
 * (DISPLAY, which clears it) — mirrors the file-level xregs in vga/sys/pix.c. */
static uint16_t xregs[16];

bool main_xreg_1(uint8_t channel, uint8_t address, uint16_t word)
{
    if (channel == 0)
    {
        xregs[address & 0x0F] = word;
        if (address == 0)
        {
            bool ok = vga_set_canvas(word);
            memset(xregs, 0, sizeof(xregs)); /* fresh state per pix.c */
            return ok;
        }
        if (address == 1)
        {
            /* Mode select (xregs[1]); params at addresses 2.. were stored first
             * by the high->low dispatch. Mirrors vga main_prog, then clears the
             * registers so the next program starts fresh. */
            bool ok;
            switch (word)
            {
            case 0:
                ok = term_prog(xregs);
                break;
            case 1:
                ok = mode1_prog(xregs);
                break;
            case 2:
                ok = mode2_prog(xregs);
                break;
            case 3:
                ok = mode3_prog(xregs);
                break;
            case 4:
                ok = mode4_prog(xregs);
                break;
            case 5:
                ok = mode5_prog(xregs);
                break;
            default:
                ok = false; /* all VGA modes modeled */
                break;
            }
            memset(xregs, 0, sizeof(xregs));
            return ok;
        }
        return true; /* parameter register stored */
    }
    if (channel == 0x0F)
    {
        /* VGA control channel — RIA-private (guest writes NAK in pix_api_xreg).
         * Mirrors vga/sys/pix.c pix_ch15_xreg; only registers with an emu analog
         * are handled. */
        if (address == 0x00) /* DISPLAY: also resets to the console canvas */
        {
            vga_set_canvas(vga_canvas_console); /* == firmware vga_xreg_canvas(NULL) */
            term_RIS_no_clear();                /* preserve-screen terminal RIS */
            memset(xregs, 0, sizeof(xregs));
            return true;
        }
        return false; /* no emu analog for the other control registers */
    }
    /* Channels 1-14 reach external bus devices with no ACK; the emulator has none,
     * so a no-op success. */
    return true;
}

/* The 6502 syscall op -> handler table. A runtime array (not a switch) so the dir
 * slots can be swapped between the emu's host handlers and the REAL firmware
 * fat_api_* (ria/api/fat.c) when --tmpdrive mounts a RAM FatFs. The dir slots
 * default to host below; main_dir_ops_set() swaps them. */
typedef bool (*api_op_fn)(void);
static api_op_fn api_ops[0x40] = {
    [0x01] = pix_api_xreg,
    [0x02] = atr_api_phi2,
    [0x03] = atr_api_code_page,
    [0x04] = atr_api_lrand,
    [0x06] = atr_api_errno_opt,
    [0x08] = pro_api_argv,
    [0x09] = pro_api_exec,
    [0x0A] = atr_api_get,
    [0x0B] = atr_api_set,
    [0x0D] = clk_api_tzset,
    [0x0E] = clk_api_tzquery,
    [0x0F] = clk_api_clock,
    [0x10] = clk_api_get_res,
    [0x11] = clk_api_get_time,
    [0x12] = clk_api_set_time,
    [0x14] = std_api_open,
    [0x15] = std_api_close,
    [0x16] = std_api_read_xstack,
    [0x17] = std_api_read_xram,
    [0x18] = std_api_write_xstack,
    [0x19] = std_api_write_xram,
    [0x1A] = std_api_lseek_cc65,
    [0x1B] = msc_api_unlink,
    [0x1C] = msc_api_rename,
    [0x1D] = std_api_lseek_llvm,
    [0x1E] = std_api_syncfs,
    [0x1F] = msc_api_stat,
    [0x20] = msc_api_opendir,
    [0x21] = msc_api_readdir,
    [0x22] = msc_api_closedir,
    [0x23] = msc_api_telldir,
    [0x24] = msc_api_seekdir,
    [0x25] = msc_api_rewinddir,
    [0x26] = msc_api_chmod,
    [0x27] = msc_api_utime,
    [0x28] = msc_api_mkdir,
    [0x29] = msc_api_chdir,
    [0x2A] = msc_api_chdrive,
    [0x2B] = msc_api_getcwd,
    [0x2C] = msc_api_setlabel,
    [0x2D] = msc_api_getlabel,
    [0x2E] = msc_api_getfree,
    [0x30] = rln_api_lastkey,
    [0x31] = rln_api_peek,
    [0x32] = rln_api_poke,
    [0x3A] = clk_api_gmtime,
    [0x3B] = clk_api_localtime,
    [0x3C] = clk_api_mktime,
    [0x3D] = clk_api_strftime,
    [0x3E] = clk_api_time_set,
    [0x3F] = clk_api_time_get,
};

/* Swap the dir op slots: the firmware's own fat_api_* (over the RAM FatFs) when
 * fat, else the emu's host handlers. */
void main_dir_ops_set(bool fat)
{
    static const struct
    {
        uint8_t op;
        api_op_fn host, fat;
    } slots[] = {
        {0x1B, msc_api_unlink, fat_api_unlink},
        {0x1C, msc_api_rename, fat_api_rename},
        {0x1F, msc_api_stat, fat_api_stat},
        {0x20, msc_api_opendir, fat_api_opendir},
        {0x21, msc_api_readdir, fat_api_readdir},
        {0x22, msc_api_closedir, fat_api_closedir},
        {0x23, msc_api_telldir, fat_api_telldir},
        {0x24, msc_api_seekdir, fat_api_seekdir},
        {0x25, msc_api_rewinddir, fat_api_rewinddir},
        {0x26, msc_api_chmod, fat_api_chmod},
        {0x27, msc_api_utime, fat_api_utime},
        {0x28, msc_api_mkdir, fat_api_mkdir},
        {0x29, msc_api_chdir, fat_api_chdir},
        {0x2A, msc_api_chdrive, fat_api_chdrive},
        {0x2B, msc_api_getcwd, fat_api_getcwd},
        {0x2C, msc_api_setlabel, fat_api_setlabel},
        {0x2D, msc_api_getlabel, fat_api_getlabel},
        {0x2E, msc_api_getfree, fat_api_getfree},
    };
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++)
        api_ops[slots[i].op] = fat ? slots[i].fat : slots[i].host;
}

/* The registry api_task dispatches through; the firmware's is the switch in
 * main.c. Returns true if the op has more work (api_working) and should be
 * re-dispatched, false once it has returned to the 6502. */
bool main_api(uint8_t operation)
{
    api_op_fn fn = operation < 0x40 ? api_ops[operation] : NULL;
    return fn ? fn() : api_return_errno(API_ENOSYS);
}

static const std_driver_t std_drivers[] = {
    {rom_std_handles, rom_std_open, rom_std_close, rom_std_read, NULL, NULL, rom_std_lseek},
    {tmp_std_handles, fat_std_open, fat_std_close, fat_std_read, fat_std_write, fat_std_sync, fat_std_lseek},
    {msc_std_handles, msc_std_open, msc_std_close, msc_std_read, msc_std_write, msc_std_sync, msc_std_lseek},
};

const std_driver_t *main_std_drivers(size_t *count)
{
    *count = sizeof(std_drivers) / sizeof(std_drivers[0]);
    return std_drivers;
}
