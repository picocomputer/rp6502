/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/main.h"
#include "emu/api/oem.h"
#include "emu/api/pro.h"
#include "emu/aud/aud.h"
#include "emu/dbg/dbg.h"
#include "emu/api/hostfs.h"
#include "emu/mon/rom.h"
#include "emu/sys/com.h"
#include "emu/sys/cpu.h"
#include "emu/sys/mem.h"
#include "emu/sys/vga.h"
#include "emu/sys/via.h"
#include "emu/sys/xreg.h"
#include "emu/chips/rp6502.h"
/* Firmware handler decls, ria/-qualified: a bare "api/std.h" or "aud/aud.h" from
 * this root file would bind to the emu/api/ and emu/aud/ shadows beside it, not
 * the firmware headers that declare aud_init and the *_api_* op handlers. */
#include "ria/api/api.h"
#include "ria/api/atr.h"
#include "ria/api/std.h"
#include "ria/api/dir.h"
#include "ria/api/clk.h"
#include "ria/aud/aud.h"
#include "ria/str/rln.h"
#include "term/term.h" /* no emu/term shadow; resolves to vga/term */
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Machine-global run state                                            */
/* ------------------------------------------------------------------ */

/* The virtual master clock, in 1/8-of-a-256 MHz-tick units (2048/µs) so the PHI2
 * fractional divider lands on an integer per-cycle step. Wraps in centuries. */
static uint64_t master_8;

static int s_exit_code;

/* Diagnostic: should advance at 60 Hz. */
static unsigned long s_frame_count;

/* Absolute, never reset per frame — feeds the exact deadline math below. */
static uint64_t scanline_n;

int main_exit_code(void) { return s_exit_code; }
void main_set_exit_code(int code) { s_exit_code = code; }
unsigned long main_frame_count(void) { return s_frame_count; }
uint64_t main_clock_8(void) { return master_8; }

void main_init(void)
{
    pro_init();
    cpu_init(); /* default PHI2 (--phi2 reapplies after main_init) */
    master_8 = 0; /* the virtual master clock starts at boot */
    scanline_n = 0;
    s_frame_count = 0;
    aud_init(); /* fill the shared sine table (no PWM off-device) */
    ria_reset();
    com_reset();        /* cold boot: flush queued input (per-exec keeps type-ahead) */
    oem_reset();        /* cold boot: default code page 437 (exec preserves the page) */
    vga_boot_console(); /* font_init loads that same 437 default into the font */
    cpu_reset();
    via_reset(); /* the VIA shares the 6502's RESB, so a CPU reset clears it */
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

    const int vsync_line = vga_vsync_scanline();
    const int canvas_h = vga_canvas_height(); /* visible region; snapshot for the frame */
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
    aud_task(); /* generate this frame's audio from the active RIA device */

    /* An exec committed this frame: load the new program and restart the CPU,
     * keeping the master clock and the argv pro_api_exec stored. The terminal
     * and VGA state are NOT reset — the new program's output appends to the
     * existing screen, as on real hardware. */
    const char *exec_path = pro_take_exec();
    if (exec_path)
    {
        if (!rom_load(exec_path))
        {
            fprintf(stderr, "rp6502-emu: exec failed to load '%s'\n", exec_path);
            cpu_set_halted(true);
            s_exit_code = 1;
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

void main_run_frame(void) { run_frame(true); }

/* Run one frame WITHOUT rendering — a catch-up frame the pacer will not present.
 * CPU/chip/timing/vsync all advance; only the per-scanline pixel work is skipped
 * (most of the per-frame cost), so catching up after a slow/stalled host is cheap. */
void main_run_frame_norender(void) { run_frame(false); }

/* ------------------------------------------------------------------ */
/* xreg (op 0x01): marshal device/channel/address + words off the xstack */
/* ------------------------------------------------------------------ */

/* The i-th xreg data word (target address+i) sits at xstack[SIZE-5-2i]. */
static uint16_t word_at(int i)
{
    uint16_t word;
    memcpy(&word, &xstack[XSTACK_SIZE - 5 - 2 * i], sizeof(word));
    return word;
}

static bool std_xreg(void)
{
    uint8_t device = xstack[XSTACK_SIZE - 1];
    uint8_t channel = xstack[XSTACK_SIZE - 2];
    uint8_t address = xstack[XSTACK_SIZE - 3];
    int count = (int)((XSTACK_SIZE - xstack_ptr - 3) / 2);
    bool aligned = (xstack_ptr & 1) != 0;
    xstack_ptr = XSTACK_SIZE; /* args consumed; nothing below reads xstack_ptr */
    if (!aligned || count < 1 || count > XSTACK_SIZE / 2 ||
        device > 7 || channel > 15)
        return api_return_errno(API_EINVAL);
    /* VGA control channel ($F) is RIA-private while VGA is connected (always,
     * in the emulator), so a write NAKs (mirrors ria/sys/pix.c). */
    if (device == 1 && channel == 0xF)
        return api_return_errno(API_EACCES);
    /* word[i] lives at xstack[SIZE-5-2i] and targets address+i. Hardware
     * dispatch order: a VGA channel-0 multi-word call starting at address 0
     * sends the canvas word (address 0) first so it can't clear later mode
     * programming; every other word follows high address -> low. This makes
     * a register that consumes earlier ones (e.g. the term mode word at
     * address 1) land after its parameters. */
    bool canvas_first = (device == 1 && channel == 0 && address == 0 && count > 1);
    if (canvas_first && !xreg_write(device, channel, address, word_at(0)))
        return api_return_errno(API_EINVAL);
    for (int i = count - 1; i >= (canvas_first ? 1 : 0); i--)
    {
        /* PIX_DEVICE_RIA (device 0) holds the address constant (last-wins);
         * only the VGA/non-RIA path walks address+i. */
        uint8_t reg = device ? (uint8_t)(address + i) : address;
        if (!xreg_write(device, channel, reg, word_at(i)))
            return api_return_errno(API_EINVAL);
    }
    return api_return_ax(0);
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

/* The 6502 syscall op -> handler table. A runtime array (not a switch) so the dir
 * slots can be swapped between the emu's host handlers and the REAL firmware
 * dir_api_* (ria/api/dir.c) when --tmpdrive mounts a RAM FatFs. The dir slots
 * default to host below; main_dir_ops_set() swaps them. */
typedef bool (*api_op_fn)(void);
static api_op_fn api_ops[0x40] = {
    [0x01] = std_xreg,
    [0x02] = atr_api_phi2,
    [0x03] = atr_api_code_page,
    [0x04] = atr_api_lrand,
    [0x06] = atr_api_errno_opt,
    [0x08] = pro_api_argv,
    [0x09] = pro_api_exec,
    [0x0A] = atr_api_get,
    [0x0B] = atr_api_set,
    [0x14] = std_api_open,
    [0x15] = std_api_close,
    [0x16] = std_api_read_xstack,
    [0x17] = std_api_read_xram,
    [0x18] = std_api_write_xstack,
    [0x19] = std_api_write_xram,
    [0x1A] = std_api_lseek_cc65,
    [0x1B] = hostfs_api_unlink,
    [0x1C] = hostfs_api_rename,
    [0x1D] = std_api_lseek_llvm,
    [0x1E] = std_api_syncfs,
    [0x1F] = hostfs_api_stat,
    [0x20] = hostfs_api_opendir,
    [0x21] = hostfs_api_readdir,
    [0x22] = hostfs_api_closedir,
    [0x23] = hostfs_api_telldir,
    [0x24] = hostfs_api_seekdir,
    [0x25] = hostfs_api_rewinddir,
    [0x26] = hostfs_api_chmod,
    [0x27] = hostfs_api_utime,
    [0x28] = hostfs_api_mkdir,
    [0x29] = hostfs_api_chdir,
    [0x2A] = hostfs_api_chdrive,
    [0x2B] = hostfs_api_getcwd,
    [0x2C] = hostfs_api_setlabel,
    [0x2D] = hostfs_api_getlabel,
    [0x2E] = hostfs_api_getfree,
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

/* Swap the dir op slots: the firmware's own dir_api_* (over the RAM FatFs) when
 * fat, else the emu's host handlers. */
void main_dir_ops_set(bool fat)
{
    static const struct
    {
        uint8_t op;
        api_op_fn host, fat;
    } slots[] = {
        {0x1B, hostfs_api_unlink, dir_api_unlink},
        {0x1C, hostfs_api_rename, dir_api_rename},
        {0x1F, hostfs_api_stat, dir_api_stat},
        {0x20, hostfs_api_opendir, dir_api_opendir},
        {0x21, hostfs_api_readdir, dir_api_readdir},
        {0x22, hostfs_api_closedir, dir_api_closedir},
        {0x23, hostfs_api_telldir, dir_api_telldir},
        {0x24, hostfs_api_seekdir, dir_api_seekdir},
        {0x25, hostfs_api_rewinddir, dir_api_rewinddir},
        {0x26, hostfs_api_chmod, dir_api_chmod},
        {0x27, hostfs_api_utime, dir_api_utime},
        {0x28, hostfs_api_mkdir, dir_api_mkdir},
        {0x29, hostfs_api_chdir, dir_api_chdir},
        {0x2A, hostfs_api_chdrive, dir_api_chdrive},
        {0x2B, hostfs_api_getcwd, dir_api_getcwd},
        {0x2C, hostfs_api_setlabel, dir_api_setlabel},
        {0x2D, hostfs_api_getlabel, dir_api_getlabel},
        {0x2E, hostfs_api_getfree, dir_api_getfree},
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
