/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Which way each walk goes.
 *
 * A machine is a list of rows and core/sys/sys.c is what walks it: init, run
 * and the two task columns forward, stop and break backward. Every one of
 * those directions is load-bearing somewhere -- the Pocket's roster puts fs
 * before std so the reversal closes files before the driver that owns them,
 * and the RIA's puts com near the end so a break's newline lands after what
 * the other breaks printed -- and none of it was witnessed by anything. Turn
 * DRIVERS_REVERSE into DRIVERS_FORWARD and every other suite stays green.
 *
 * sys.c is compiled in rather than linked: sys_on_stop and sys_on_break are
 * static, because performing a fan-out is the loop's alone, and this is the
 * one caller that is not sys_commit. The roster beside this file is what it
 * finds for "drivers.h" -- the test links no machine, so no other roster is
 * on the path to shadow it.
 */

#include "core/sys/sys.c"

#include "utest.h"

#include <string.h>

UTEST_MAIN();

/* Each hook writes its row's letter and its column's, in the order called. */
static char walked[64];
static void note(const char *what)
{
    if (strlen(walked) + 2 < sizeof walked)
        strcat(walked, what);
}

/* clang-format off */
void a_init(void) { note("ai"); } void a_task(void) { note("at"); }
void a_io(void)   { note("ao"); } void a_run(void)  { note("ar"); }
void a_stop(void) { note("as"); } void a_break(void){ note("ab"); }
void b_init(void) { note("bi"); } void b_task(void) { note("bt"); }
void b_io(void)   { note("bo"); } void b_run(void)  { note("br"); }
void b_stop(void) { note("bs"); } void b_break(void){ note("bb"); }
void c_init(void) { note("ci"); } void c_task(void) { note("ct"); }
void c_io(void)   { note("co"); } void c_run(void)  { note("cr"); }
void c_stop(void) { note("cs"); } void c_break(void){ note("cb"); }
/* clang-format on */

/* The line this machine has not got. sys.c is the one file that lowers and
 * raises it, so the test answers for it and reads it back where it matters. */
static bool resb_down;
void resb_init(void) { resb_down = true; }
void resb_assert(void) { resb_down = true; }
void resb_release(void) { resb_down = false; }
bool resb_running(void) { return !resb_down; }

/* Each case establishes the state it needs, so they hold in any order. */
static void from_stopped(void)
{
    sys_stop();
    sys_commit();
    walked[0] = '\0';
}

UTEST(roster, init_walks_the_list_forward)
{
    walked[0] = '\0';
    sys_init();
    ASSERT_STREQ("aibici", walked);
}

UTEST(roster, a_run_walks_forward)
{
    from_stopped();
    sys_run();
    sys_commit();
    ASSERT_STREQ("arbrcr", walked);
}

/* Backward, so a row is put away before whatever came up before it. */
UTEST(roster, a_stop_walks_the_run_in_reverse)
{
    from_stopped();
    sys_run();
    sys_commit();
    walked[0] = '\0';
    sys_stop();
    sys_commit();
    ASSERT_STREQ("csbsas", walked);
}

/* The performed-on-the-spot stop takes the same direction as the deferred one;
 * proc_boot reaches it with a program's RAM about to be written over. */
UTEST(roster, stop_now_walks_the_same_way)
{
    from_stopped();
    sys_run();
    sys_commit();
    walked[0] = '\0';
    sys_stop_now();
    ASSERT_STREQ("csbsas", walked);
    ASSERT_FALSE(sys_active());
}

/* A break is a teardown too, and follows the stop it implies. */
UTEST(roster, a_break_walks_in_reverse_after_the_stop)
{
    from_stopped();
    sys_run();
    sys_commit();
    walked[0] = '\0';
    sys_break_request();
    sys_commit();
    ASSERT_STREQ("csbsascbbbab", walked);
}

/* Two columns and two walks. A machine whose file operations block re-enters
 * sys_task while a transfer completes, so a row whose work may touch a
 * filesystem is in the other one; putting it in the wrong column is invisible
 * on a machine that calls the two back to back. */
UTEST(roster, the_task_columns_are_two_walks)
{
    from_stopped();
    sys_task();
    ASSERT_STREQ("atbtct", walked);
    walked[0] = '\0';
    sys_io_task();
    ASSERT_STREQ("aoboco", walked);
}

/* RESB is sys.c's own, and no row could hold it: down before the walk, down
 * again in every ask, up only once the run fan-out has finished. */
UTEST(roster, the_reset_line_outlives_the_run_walk)
{
    from_stopped();
    ASSERT_FALSE(resb_running());
    sys_run();
    ASSERT_FALSE(resb_running()); /* the ask is not the doing */
    sys_commit();
    ASSERT_TRUE(resb_running());
    sys_stop();
    ASSERT_FALSE(resb_running()); /* inside the ask, ahead of the walk */
}
