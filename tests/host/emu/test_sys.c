/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * api_run is one of the run hooks, and it sets STEP0 at $FFE5 to 1. No 6502
 * code runs and no savestate is loaded in these cases, so nothing else writes
 * $FFE5, and a 1 there after sys_commit means the run fan-out ran.
 * sys_active() cannot show this, because it is already true once sys_run has
 * been called.
 */

#include "core/ria/regs.h"
#include "core/sys/sys.h"
#include "emu_boot.h"

UTEST_MAIN_EMU();

static void forget_the_run(void) { REGS(0xFFE5) = 0; }
static bool the_run_walked(void) { return REGS(0xFFE5) == 1; }

UTEST(sys, a_run_then_a_commit_brings_the_drivers_up)
{
    sys_stop();
    sys_commit();
    forget_the_run();

    sys_run();
    ASSERT_TRUE(sys_active());
    ASSERT_FALSE(the_run_walked());

    sys_commit();
    ASSERT_TRUE(sys_active());
    ASSERT_TRUE(the_run_walked());
}

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

UTEST(sys, the_asks_repeat_without_effect)
{
    sys_stop();
    sys_commit();

    sys_run();
    sys_run();
    sys_commit();
    forget_the_run();
    sys_run();
    sys_commit();
    ASSERT_TRUE(sys_active());
    ASSERT_FALSE(the_run_walked());

    sys_stop();
    sys_stop();
    sys_commit();
    ASSERT_FALSE(sys_active());
}

UTEST(sys, the_latch_goes_back_without_driving_anything)
{
    sys_run();
    sys_commit();
    ASSERT_TRUE(sys_active());

    sys_latch_t running_here;
    sys_latch_get(&running_here);
    ASSERT_TRUE(running_here.state != 0);

    sys_stop();
    sys_commit();
    ASSERT_FALSE(sys_active());
    forget_the_run();

    ASSERT_TRUE(sys_latch_apply(&running_here));
    ASSERT_TRUE(sys_active());
    ASSERT_FALSE(the_run_walked());

    sys_latch_t stopped_here;
    sys_stop();
    sys_commit();
    sys_latch_get(&stopped_here);
    ASSERT_TRUE(sys_latch_apply(&running_here));
    forget_the_run();
    ASSERT_TRUE(sys_latch_apply(&stopped_here));
    ASSERT_FALSE(sys_active());
    ASSERT_FALSE(the_run_walked());
}

UTEST(sys, a_machine_that_could_not_have_existed_is_refused)
{
    sys_run();
    sys_commit();
    sys_latch_t was;
    sys_latch_get(&was);

    sys_latch_t bad = was;
    for (uint8_t s = 1; s <= 3; s += 2) /* starting, stopping */
    {
        bad.state = s;
        ASSERT_FALSE(sys_latch_apply(&bad));
    }
    bad.state = 4;
    ASSERT_FALSE(sys_latch_apply(&bad));

    bad.state = 0;
    bad.held = false;
    ASSERT_FALSE(sys_latch_apply(&bad));

    sys_latch_t now;
    sys_latch_get(&now);
    ASSERT_EQ((int)now.state, (int)was.state);
    ASSERT_EQ((int)now.held, (int)was.held);
    ASSERT_TRUE(sys_active());
}

UTEST(sys, the_header_bit_follows_the_state)
{
    sys_run();
    sys_commit();
    sys_latch_t got;
    sys_latch_get(&got);
    ASSERT_FALSE(got.held);
    sys_latch_t stopped = {.state = 0, .breaking = false, .held = true};
    ASSERT_TRUE(sys_latch_apply(&stopped));
    sys_latch_get(&got);
    ASSERT_TRUE(got.held);
    ASSERT_FALSE(sys_active());
}
