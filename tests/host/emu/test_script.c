/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The script's clock. A script is the only thing that advances the machine
 * under --script, and script_task hands control back exactly when it owes a
 * frame — so `run 600` is six hundred frames, not "at least six hundred".
 * The .txt suites next door prove the verbs; this proves the count, which
 * nothing observable from inside a script can.
 */

#include "host/sokol/script.h"
#include "core/sys/rom.h"
#include "core/sys/sys.h"
#include "emu_boot.h"
#include <stdio.h>

/* main.c's loop, which is the contract: one frame per script_task that asks. */
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
    unsigned long before = sys_frame_count();
    while (script_running())
    {
        script_task();
        if (script_running())
            script_needs_pixels() ? sys_run_frame() : sys_run_frame_norender();
    }
    return sys_frame_count() - before;
}

UTEST(script, run_is_exact)
{
    ASSERT_EQ(frames_for("run 0\n"), 0ul);
    ASSERT_EQ(frames_for("run 1\n"), 1ul);
    ASSERT_EQ(frames_for("run\n"), 1ul); /* the default */
    ASSERT_EQ(frames_for("run 600\n"), 600ul);
    /* And they add up, rather than each rounding up to the next poll. */
    ASSERT_EQ(frames_for("run 100\nrun 200\nrun 300\n"), 600ul);
}

/* A command that does not wait costs nothing, so a script's frame count is the
 * sum of its run/wait budgets and stays readable as one. */
UTEST(script, checks_are_free)
{
    ASSERT_EQ(frames_for("# nothing but a comment\n\n"), 0ul);
    ASSERT_EQ(frames_for("pad 0 connect\npad 0 press start\npad 0 disconnect\n"), 0ul);
    ASSERT_EQ(frames_for("run 5\npoke xram:$0000 $A5\npeek xram:$0000 $A5\nrun 5\n"), 10ul);
}

/* A budget is spent the same way: the run gives up on the frame it named,
 * neither early nor a poll late. */
UTEST(script, budget_is_exact)
{
    ASSERT_EQ(frames_for("wait \"no program says this\" 5\n"), 5ul);
    ASSERT_EQ(frames_for("expect-exit 0 7\n"), 7ul);
}

/* Waiting on memory is the handshake a generated program answers, so it has to
 * cost what it says: nothing when the byte is already there, and the whole
 * budget when it never arrives. */
UTEST(script, waiting_on_memory_is_exact)
{
    ASSERT_EQ(frames_for("poke ram:$0200 $A5\nwait ram:$0200 $A5 9\n"), 0ul);
    ASSERT_EQ(frames_for("poke ram:$0200 $00\nwait ram:$0200 $A5 5\n"), 5ul);
}

/* Skipping the pixels through the middle of a run must not change what a frame
 * is: the counter, vsync and every task still run, so the count is the same as
 * a run whose frames are each observed. */
UTEST(script, the_render_gate_keeps_the_count)
{
    ASSERT_EQ(frames_for("run 30\n"), 30ul);
    ASSERT_EQ(frames_for("run 10\npoke ram:$0200 $01\nrun 10\nrun 10\n"), 30ul);
}

/* Answers are off until a script asks, so a preamble costs no frames and says
 * nothing, and turning them on is itself a command like any other. */
UTEST(script, replies_cost_no_frames)
{
    ASSERT_EQ(frames_for("reply\nreply off\n"), 0ul);
    ASSERT_EQ(frames_for("reply\nrun 4\n"), 4ul);
}

UTEST_MAIN_EMU();
