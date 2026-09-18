/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/dap/dbg.h"
#include "core/sys/sys.h"
#include "core/wdc/sram.h"
#include "host/host.h"
#include "core/vga/vga_emu.h"
#include "core/hid/vtkeys.h"
#include "core/aud/mix.h"
#include "core/aud/bel.h"
#include "emu_boot.h"
#include <string.h>

static uint16_t entry_pc(void)
{
    return (uint16_t)(sram[0xFFFC] | (sram[0xFFFD] << 8));
}

static bool load(void)
{
    return emu_restart(TEST_FIXTURE);
}

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

UTEST(dbg, a_pause_holds_the_level_but_a_mach_stop_does_not)
{
    ASSERT_TRUE(load());

    sys_stop();
    sys_commit();
    bel_add(&bel_teletype);
    emu_frames(1);
    int n = aud_render(g_out, 800);
    ASSERT_GT(n, 0);
    ASSERT_FALSE(held_at(g_out[0], g_out[1]));
    float last_l = g_out[(n - 1) * 2], last_r = g_out[(n - 1) * 2 + 1];

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

    dbg_continue();
    ASSERT_EQ(aud_render(g_out, 800), 800);
    ASSERT_FALSE(held_at(last_l, last_r));

    disarm();
    emu_frames(60);
}

static int wp_writes, wp_reads, wp_reads_above_ram;

static void wp_tap(uint16_t addr, uint8_t val, bool is_write)
{
    (void)val;
    if (is_write)
        wp_writes++;
    else if (++wp_reads, addr > SRAM_MMAP_HI)
        wp_reads_above_ram++;
}

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

    ASSERT_GT(wp_writes, 0);
    ASSERT_GT(wp_reads, 0);
    ASSERT_EQ(wp_reads_above_ram, 0);

    disarm();
}

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
    ASSERT_TRUE(sys_running());

    emu_frames(1);
    ASSERT_TRUE(dbg_is_stopped());
    ASSERT_EQ((int)dbg_stop_pc(), (int)entry);

    disarm();
}

UTEST(dbg, step_advances_one_instruction)
{
    ASSERT_TRUE(load());
    uint16_t entry = entry_pc();
    dbg_add_breakpoint(entry);
    dbg_set_active(true);
    emu_frames(1);
    ASSERT_TRUE(dbg_is_stopped());

    dbg_remove_breakpoint(entry);
    dbg_step(DBG_STEP_INSTR);
    emu_frames(1);

    ASSERT_TRUE(dbg_is_stopped());
    ASSERT_EQ(dbg_stop_reason(), (int)DBG_REASON_STEP);
    ASSERT_NE((int)dbg_stop_pc(), (int)entry);

    disarm();
}

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

UTEST(dbg, a_stop_holds_the_cpu_and_not_the_screen)
{
    ASSERT_TRUE(load());
    vga_set_framebuffer(fb);
    memset(fb, 0, sizeof(fb));
    uint32_t untouched = frame_crc();
    dbg_set_active(true);

    dbg_add_breakpoint(entry_pc());
    emu_frames(1);
    ASSERT_TRUE(dbg_is_stopped());
    uint32_t console_blank = frame_crc();
    ASSERT_NE(console_blank, untouched);
    const unsigned long held_at = vga_frame_count();
    for (int i = 0; i < 200000; i++)
    {
        sys_task();
        sys_io_task();
        sys_commit();
    }
    ASSERT_TRUE(dbg_is_stopped());
    ASSERT_EQ(vga_frame_count(), held_at);
    ASSERT_EQ(frame_crc(), console_blank);

    dbg_clear_breakpoints();
    dbg_continue();
    emu_frames(60);
    dbg_request_break();
    emu_frames(1);
    ASSERT_TRUE(dbg_is_stopped());

    emu_frames(1);
    ASSERT_TRUE(dbg_is_stopped());
    ASSERT_NE(frame_crc(), console_blank);

    disarm();
    vga_set_framebuffer(NULL);
}

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

    /* Adventure asks a yes-or-no question about instructions when it starts
     * and another to confirm "quit", so pasting "no", "quit" and "yes" ends the
     * program. */
    vtkeys_paste("no\nquit\nyes\n");
    for (int i = 0; i < 600 && sys_running(); i++)
        emu_frames(1);
    ASSERT_FALSE(sys_running());
    ASSERT_FALSE(dbg_is_stopped());

    disarm();
}

UTEST_MAIN_EMU()
