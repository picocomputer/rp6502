/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Debugger engine (dbg.c): the run/stop/step + address-breakpoint logic that
 * main.c consults and that both the DAP adapter and the on-screen ImGui debugger
 * drive. Exercised headlessly against adventure.rp6502 — no window required.
 */

#include "core/dap/dbg.h"
#include "core/sys/sys.h"
#include "core/wdc/sram.h"
#include "host/host.h"
#include "core/wdc/resb.h"
#include "core/vga/vga_emu.h"
#include "core/hid/vtkeys.h"
#include "core/aud/mix.h"
#include "core/aud/bel.h"
#include "emu_boot.h"
#include <string.h>

/* The first instruction the CPU fetches after reset = the RESET vector target. */
static uint16_t entry_pc(void)
{
    return (uint16_t)(sram[0xFFFC] | (sram[0xFFFD] << 8));
}

static bool load(void)
{
    return emu_restart(TEST_FIXTURE);
}

/* Leave the engine inert so a later test runs normally. */
static void disarm(void)
{
    dbg_continue();
    dbg_clear_breakpoints();
    dbg_set_active(false);
}

static float g_out[800 * 2];

static bool held_at(float l, float r)
{
    for (int i = 0; i < 800; i++)
        if (g_out[i * 2] != l || g_out[i * 2 + 1] != r)
            return false;
    return true;
}

/* A debugger pause holds the level: the machine is stopped for someone to
 * read it, the synth does not run, and once the device has taken what the
 * machine had made it keeps playing the last of it -- not silence, which is
 * a click at each edge. A sys_stop is the opposite: audio plays right
 * through it, which is how the bell rings between programs. */
UTEST(dbg, a_pause_holds_the_level_but_a_mach_stop_does_not)
{
    ASSERT_TRUE(load());

    /* Stopped machine, ringing bell: sys_stop does not silence. */
    sys_stop();
    sys_commit();
    bel_add(&bel_teletype);
    emu_frames(1);
    int n = aud_render(g_out, 800);
    ASSERT_GT(n, 0);
    ASSERT_FALSE(held_at(g_out[0], g_out[1])); /* a ringing bell moves */
    float last_l = g_out[(n - 1) * 2], last_r = g_out[(n - 1) * 2 + 1];

    /* Held in the debugger: the machine makes nothing, and the last sample
     * it made is every sample after. */
    dbg_set_active(true);
    dbg_note_stop(entry_pc());
    ASSERT_TRUE(dbg_is_stopped());
    while ((n = aud_render(g_out, 800)) > 0)
    {
        last_l = g_out[(n - 1) * 2];
        last_r = g_out[(n - 1) * 2 + 1];
    }
    ASSERT_TRUE(held_at(last_l, last_r));
    emu_frames(1);
    ASSERT_EQ(aud_render(g_out, 800), 0);
    ASSERT_TRUE(held_at(last_l, last_r));

    /* Resume and it picks the note back up -- the synth kept its state. */
    dbg_continue();
    ASSERT_EQ(aud_render(g_out, 800), 800);
    ASSERT_FALSE(held_at(last_l, last_r));

    disarm();
    emu_frames(60); /* play the bell out */
}

/* Watchpoint tap: count what the bus hook reports, split by direction, and flag any
 * read above the SRAM's window. */
static int wp_writes, wp_reads, wp_reads_above_ram;

static void wp_tap(uint16_t addr, uint8_t val, bool is_write)
{
    (void)val;
    if (is_write)
        wp_writes++;
    else if (++wp_reads, addr > SRAM_MMAP_HI)
        wp_reads_above_ram++;
}

/* Watchpoints (data breakpoints) are DAP-only, so nothing else covers the bus hook.
 * It reports every write, but only the reads the SRAM actually drove: the reset
 * vector at $FFFC and the API trampoline at $FFF0 are the RIA answering, so a frame
 * that fetches both must still report no read above SRAM_MMAP_HI. */
UTEST(dbg, watchpoints_see_only_sram_reads)
{
    ASSERT_TRUE(load());
    dbg_clear_breakpoints();
    dbg_set_active(true);
    wp_writes = wp_reads = wp_reads_above_ram = 0;
    dbg_set_watch_cb(wp_tap);
    dbg_watch_armed = 1;

    emu_frames(1);

    dbg_watch_armed = 0;
    dbg_set_watch_cb(NULL);

    ASSERT_GT(wp_writes, 0);          /* the program stores */
    ASSERT_GT(wp_reads, 0);           /* and fetches from RAM */
    ASSERT_EQ(wp_reads_above_ram, 0); /* but never a read a device drove */

    disarm();
}

/* A breakpoint at the entry point stops the CPU on its very first instruction,
 * before any program effect — reason BREAKPOINT, PC = entry. */
UTEST(dbg, breakpoint_stops_at_entry)
{
    ASSERT_TRUE(load());
    uint16_t entry = entry_pc();
    dbg_clear_breakpoints();
    dbg_add_breakpoint(entry);
    dbg_set_active(true);

    emu_frames(1);

    ASSERT_TRUE(dbg_is_stopped());
    ASSERT_EQ((int)dbg_stop_pc(), (int)entry);
    ASSERT_EQ(dbg_stop_reason(), (int)DBG_REASON_BREAKPOINT);
    ASSERT_TRUE(resb_running()); /* stopped, not exited */

    /* Held: while stopped, further frames do not advance the CPU. */
    emu_frames(1);
    ASSERT_TRUE(dbg_is_stopped());
    ASSERT_EQ((int)dbg_stop_pc(), (int)entry);

    disarm();
}

/* From a stop, a single-instruction step runs exactly one instruction and stops
 * again at the next fetch (reason STEP, a different PC). */
UTEST(dbg, step_advances_one_instruction)
{
    ASSERT_TRUE(load());
    uint16_t entry = entry_pc();
    dbg_add_breakpoint(entry);
    dbg_set_active(true);
    emu_frames(1);
    ASSERT_TRUE(dbg_is_stopped());

    dbg_remove_breakpoint(entry); /* prove the next stop is the step, not the bp */
    dbg_step(DBG_STEP_INSTR);
    emu_frames(1);

    ASSERT_TRUE(dbg_is_stopped());
    ASSERT_EQ(dbg_stop_reason(), (int)DBG_REASON_STEP);
    ASSERT_NE((int)dbg_stop_pc(), (int)entry);

    disarm();
}

/* A pause request stops the CPU at the next instruction boundary (reason PAUSE),
 * even with no breakpoints set. */
UTEST(dbg, pause_stops_running_cpu)
{
    ASSERT_TRUE(load());
    dbg_set_active(true);
    dbg_request_pause();

    emu_frames(1);

    ASSERT_TRUE(dbg_is_stopped());
    ASSERT_EQ(dbg_stop_reason(), (int)DBG_REASON_PAUSE);

    disarm();
}

/* A break request stops the CPU at the next instruction boundary with reason
 * BREAKPOINT, even with no address breakpoint set. */
UTEST(dbg, break_request_stops_as_breakpoint)
{
    ASSERT_TRUE(load());
    dbg_set_active(true);
    dbg_request_break();

    emu_frames(1);

    ASSERT_TRUE(dbg_is_stopped());
    ASSERT_EQ(dbg_stop_reason(), (int)DBG_REASON_BREAKPOINT);

    disarm();
}

/* stopOnEntry: arming the one-shot entry stop halts at the first instruction. */
UTEST(dbg, stop_at_entry)
{
    ASSERT_TRUE(load());
    dbg_set_active(true);
    dbg_stop_at_entry();

    emu_frames(1);

    ASSERT_TRUE(dbg_is_stopped());
    ASSERT_EQ((int)dbg_stop_pc(), (int)entry_pc());
    ASSERT_EQ(dbg_stop_reason(), (int)DBG_REASON_ENTRY);

    disarm();
}

static uint32_t fb[VGA_MAX_WIDTH * VGA_MAX_HEIGHT];

static uint32_t frame_crc(void)
{
    int cw, ch;
    vga_canvas_size(&cw, &ch);
    return host_crc32(0, fb, (size_t)cw * ch * 4);
}

/* A stop holds the 6502 and nothing else. The beam keeps sweeping, so a
 * program's final prints scan out on their own and the frame counter goes on
 * advancing -- which is what a halted 65C02 sees on real hardware, and why
 * there is nothing here to sweep by hand. */
UTEST(dbg, a_stop_holds_the_cpu_and_not_the_screen)
{
    ASSERT_TRUE(load());
    vga_set_framebuffer(fb);
    memset(fb, 0, sizeof(fb));
    uint32_t untouched = frame_crc();
    dbg_set_active(true);

    dbg_add_breakpoint(entry_pc()); /* stop before anything prints */
    emu_frames(1);
    ASSERT_TRUE(dbg_is_stopped());
    uint32_t console_blank = frame_crc();
    ASSERT_NE(console_blank, untouched); /* the console scanned out */
    /* Held means the machine is held, not just the 6502. The beam stops with
     * it, so no frame completes and no vsync is latched -- otherwise a step
     * would resume into every IRQ that accrued while you were reading. */
    const unsigned long held_at = vga_frame_count();
    for (int i = 0; i < 200000; i++)
    {
        sys_task();
        sys_io_task();
        sys_commit();
    }
    ASSERT_TRUE(dbg_is_stopped());
    ASSERT_EQ(vga_frame_count(), held_at);
    ASSERT_EQ(frame_crc(), console_blank); /* the picture is the one it had */

    dbg_clear_breakpoints();
    dbg_continue();
    emu_frames(60); /* the intro banner prints and scans out */
    dbg_request_break();
    emu_frames(1);
    ASSERT_TRUE(dbg_is_stopped());

    emu_frames(1);
    ASSERT_TRUE(dbg_is_stopped());
    ASSERT_NE(frame_crc(), console_blank); /* the banner reached the pixels */

    disarm();
    vga_set_framebuffer(NULL);
}

/* Continue after a stop resumes free execution: the program runs to completion.
 * Adventure blocks on input, so drive it to its quit and let it exit, with the
 * engine no longer reporting stopped. */
UTEST(dbg, continue_runs_to_exit)
{
    ASSERT_TRUE(load());
    uint16_t entry = entry_pc();
    dbg_add_breakpoint(entry);
    dbg_set_active(true);
    emu_frames(1);
    ASSERT_TRUE(dbg_is_stopped());

    dbg_clear_breakpoints();
    dbg_continue();
    ASSERT_FALSE(dbg_is_stopped());

    /* Decline the intro prompt, "quit", then confirm "yes" -> the game exits. */
    vtkeys_paste("no\nquit\nyes\n");
    for (int i = 0; i < 600 && resb_running(); i++)
        emu_frames(1);
    ASSERT_FALSE(resb_running());
    ASSERT_FALSE(dbg_is_stopped());

    disarm();
}

UTEST_MAIN_EMU()
