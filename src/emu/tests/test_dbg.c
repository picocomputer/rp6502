/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Debugger engine (dbg.c): the run/stop/step + address-breakpoint logic that
 * sys.c consults and that both the DAP adapter and the on-screen ImGui debugger
 * drive. Exercised headlessly against adventure.rp6502 — no window required.
 */

#include "emu/dbg/dbg.h"
#include "emu/mon/rom.h"
#include "emu/sys/mem.h"
#include "emu/sys/sys.h"
#include "emu/sys/vga.h"
#include "sys/com.h"
#include "utest.h"
#include <string.h>

/* The first instruction the CPU fetches after reset = the RESET vector target. */
static uint16_t entry_pc(void)
{
    return (uint16_t)(ram[0xFFFC] | (ram[0xFFFD] << 8));
}

static bool load(void)
{
    if (!emu_rom_load(ADVENTURE_ROM))
        return false;
    emu_init();
    return true;
}

/* Push CR-terminated keystrokes to stdin, as the line editor consumes them. */
static void feed(const char *s)
{
    for (const char *p = s; *p; p++)
        com_kbd_push_byte((uint8_t)*p);
}

/* Leave the engine inert so a later test (and emu_run_frame) runs normally. */
static void disarm(void)
{
    dbg_continue();
    dbg_clear_breakpoints();
    dbg_set_active(false);
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

    emu_run_frame();

    ASSERT_TRUE(dbg_is_stopped());
    ASSERT_EQ((int)dbg_stop_pc(), (int)entry);
    ASSERT_EQ(dbg_stop_reason(), (int)DBG_REASON_BREAKPOINT);
    ASSERT_FALSE(emu_cpu_halted); /* stopped, not exited */

    /* Held: while stopped, further frames do not advance the CPU. */
    emu_run_frame();
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
    emu_run_frame();
    ASSERT_TRUE(dbg_is_stopped());

    dbg_remove_breakpoint(entry); /* prove the next stop is the step, not the bp */
    dbg_step(DBG_STEP_INSTR);
    emu_run_frame();

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

    emu_run_frame();

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

    emu_run_frame();

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

    emu_run_frame();

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
    return emu_crc32(0, fb, (size_t)cw * ch * 4);
}

/* A stop freezes the machine but not the screen: terminal output that hasn't
 * been scanned out yet (a program's final prints before the halt) is swept to
 * the framebuffer once from the frozen state. All running frames here skip
 * rendering, so ONLY the stopped-state sweeps ever touch the framebuffer. */
UTEST(dbg, stop_sweeps_pending_output_to_framebuffer)
{
    ASSERT_TRUE(load());
    vga_set_framebuffer(fb);
    memset(fb, 0, sizeof(fb));
    uint32_t untouched = frame_crc();
    dbg_set_active(true);

    dbg_add_breakpoint(entry_pc()); /* stop before anything prints */
    emu_run_frame_norender();
    ASSERT_TRUE(dbg_is_stopped());
    emu_run_frame();
    uint32_t console_blank = frame_crc();
    ASSERT_NE(console_blank, untouched); /* the sweep painted the blank console */

    dbg_clear_breakpoints();
    dbg_continue();
    for (int i = 0; i < 60; i++) /* intro banner prints; nothing rendered */
        emu_run_frame_norender();
    dbg_request_break();
    emu_run_frame_norender();
    ASSERT_TRUE(dbg_is_stopped());

    emu_run_frame();
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
    emu_run_frame();
    ASSERT_TRUE(dbg_is_stopped());

    dbg_clear_breakpoints();
    dbg_continue();
    ASSERT_FALSE(dbg_is_stopped());

    /* Decline the intro prompt, "quit", then confirm "yes" -> the game exits. */
    feed("no\rquit\ryes\r");
    for (int i = 0; i < 600 && !emu_cpu_halted; i++)
        emu_run_frame();
    ASSERT_TRUE(emu_cpu_halted);
    ASSERT_FALSE(dbg_is_stopped());

    disarm();
}

UTEST_MAIN()
