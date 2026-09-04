/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The latch: asking for a machine, and getting one.
 *
 * sys_run and sys_stop are asks. They are cheap because anywhere may make
 * them -- a syscall, a key, an interrupt on another core -- and the fan-out
 * that brings drivers up or puts them away belongs to the loop, which calls
 * sys_commit where it can afford to. What is worth pinning is what happens
 * when both are asked before that commit, because the answer decides whether
 * a driver's stop hook runs at all, and nothing else in the tree would notice
 * if it stopped running.
 *
 * The witness is api_run: it writes STEP0, and nothing else does. So a 1 in
 * $FFE5 after a commit means the run fan-out walked, and a 0 means it did
 * not. That is a sharper question than sys_active(), which only says what was
 * asked for.
 */

#include "core/ria/regs.h"
#include "core/sys/sys.h"
#include "emu_boot.h"

UTEST_MAIN_EMU();

static void forget_the_run(void) { REGS(0xFFE5) = 0; }
static bool the_run_walked(void) { return REGS(0xFFE5) == 1; }

/* Each case establishes the state it needs, so they hold in any order. */

UTEST(sys, a_run_then_a_commit_brings_the_drivers_up)
{
    sys_stop();
    sys_commit();
    forget_the_run();

    sys_run();
    ASSERT_TRUE(sys_active()); /* the ask counts before the doing */
    ASSERT_FALSE(the_run_walked());

    sys_commit();
    ASSERT_TRUE(sys_active());
    ASSERT_TRUE(the_run_walked());
}

/* The shortcut host/pico/ria/sys/ria.c depends on: a stop asked for before
 * the machine ever started is not a teardown, because there is nothing up to
 * tear down. Both walks must be skipped, not just the stop. */
UTEST(sys, a_stop_before_the_start_skips_both_fan_outs)
{
    sys_stop();
    sys_commit();
    forget_the_run();

    sys_run();
    sys_stop();
    sys_commit();

    ASSERT_FALSE(sys_active());
    ASSERT_FALSE(the_run_walked());
}

/* The other order, and the one that used to go wrong: a stop asked for on a
 * running machine is a teardown that machine owes its drivers. A run asked
 * before the commit must not cancel it -- promoting it would skip every stop
 * hook, and std_stop is where a program's open files are closed. */
UTEST(sys, a_run_asked_while_a_stop_is_owed_does_not_cancel_it)
{
    sys_run();
    sys_commit();
    ASSERT_TRUE(sys_active());

    sys_stop();
    forget_the_run();
    sys_run();
    sys_commit();

    ASSERT_FALSE(sys_active());
    ASSERT_FALSE(the_run_walked());
}

/* Both asks are idempotent, which is what lets a caller make them without
 * first asking what the machine is doing. */
UTEST(sys, the_asks_repeat_without_effect)
{
    sys_stop();
    sys_commit();

    sys_run();
    sys_run();
    sys_commit();
    forget_the_run();
    sys_run(); /* already running: no second bring-up */
    sys_commit();
    ASSERT_TRUE(sys_active());
    ASSERT_FALSE(the_run_walked());

    sys_stop();
    sys_stop();
    sys_commit();
    ASSERT_FALSE(sys_active());
}
