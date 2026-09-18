/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "host/sokol/cli/script.h"
#include "core/rom/rom.h"
#include "emu_boot.h"
#include <stdio.h>

/* This loop matches the one in main.c, which runs one frame after each
 * script_task call that leaves the script running. */
static unsigned long frames_for(const char *text)
{
    char path[512];
    snprintf(path, sizeof path, "%s/script.txt", TEST_SCRATCH);
    FILE *f = fopen(path, "w");
    if (!f)
        return (unsigned long)-1;
    fputs(text, f);
    fclose(f);

    if (!emu_restart(TEST_FIXTURE) || !script_load(path))
        return (unsigned long)-1;
    unsigned long before = vga_frame_count();
    while (script_running())
    {
        script_task();
        if (script_running())
            emu_frames(1);
    }
    return vga_frame_count() - before;
}

UTEST(script, run_is_exact)
{
    ASSERT_EQ(frames_for("run 0\n"), 0ul);
    ASSERT_EQ(frames_for("run 1\n"), 1ul);
    ASSERT_EQ(frames_for("run\n"), 1ul);
    ASSERT_EQ(frames_for("run 600\n"), 600ul);
    ASSERT_EQ(frames_for("run 100\nrun 200\nrun 300\n"), 600ul);
}

UTEST(script, checks_are_free)
{
    ASSERT_EQ(frames_for("# nothing but a comment\n\n"), 0ul);
    ASSERT_EQ(frames_for("pad 0 connect\npad 0 press start\npad 0 disconnect\n"), 0ul);
    ASSERT_EQ(frames_for("run 5\npoke xram:$0000 $A5\npeek xram:$0000 $A5\nrun 5\n"), 10ul);
}

UTEST(script, budget_is_exact)
{
    ASSERT_EQ(frames_for("wait \"no program says this\" 5\n"), 5ul);
    ASSERT_EQ(frames_for("expect-exit 0 7\n"), 7ul);
}

UTEST(script, waiting_on_memory_is_exact)
{
    ASSERT_EQ(frames_for("poke ram:$0200 $A5\nwait ram:$0200 $A5 9\n"), 0ul);
    ASSERT_EQ(frames_for("poke ram:$0200 $00\nwait ram:$0200 $A5 5\n"), 5ul);
}

UTEST(script, the_render_gate_keeps_the_count)
{
    ASSERT_EQ(frames_for("run 30\n"), 30ul);
    ASSERT_EQ(frames_for("run 10\npoke ram:$0200 $01\nrun 10\nrun 10\n"), 30ul);
}

UTEST(script, replies_cost_no_frames)
{
    ASSERT_EQ(frames_for("reply\nreply off\n"), 0ul);
    ASSERT_EQ(frames_for("reply\nrun 4\n"), 4ul);
}

UTEST_MAIN_EMU();
