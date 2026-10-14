/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/sys.h"
#include "core/sys/ria.h"
#include "core/api/arg.h"
#include "core/sys/proc.h"
#include "core/api/std.h"
#include "core/aud/mix.h"
#include "core/aud/bel.h"
#include "core/hid/vtkeys.h"
#include "core/com/com.h"
#include "core/ria/ria.h"
#include "stdsys.h"
#include "emu_boot.h"

UTEST(features, sigint_irq)
{
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));

    ASSERT_FALSE(ria_irq_asserted());

    vtkeys_ctrl_letter('c');
    ASSERT_TRUE(ria_get_sigint());
    ASSERT_FALSE(ria_get_sigint());

    vtkeys_ctrl_letter('c');
    ASSERT_FALSE(ria_irq_asserted());

    /* A write to $FFF0 also acknowledges every pending bit it enables, so the
     * SIGINT already pending does not assert IRQB once it is enabled. */
    ria_reg_write(0xFFF0, 0x40);
    ASSERT_FALSE(ria_irq_asserted());

    vtkeys_ctrl_letter('c');
    ASSERT_TRUE(ria_irq_asserted());

    uint8_t flags = ria_reg_read(0xFFF0);
    ASSERT_TRUE((flags & 0x40) != 0);
    ASSERT_FALSE(ria_irq_asserted());
}

UTEST(features, ria_tick_holds_irq_through_ack)
{
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));

    ria_reg_write(0xFFF0, 0x40);
    vtkeys_ctrl_letter('c');
    ASSERT_TRUE(ria_irq_asserted());

    uint8_t data = 0;
    ASSERT_TRUE(ria_tick(0xFFF0, true, &data));
    ASSERT_TRUE((data & 0x40) != 0);

    ASSERT_FALSE(ria_tick(0x0000, true, &data));
}

UTEST(features, launcher_chain)
{
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));

    proc_set_argv("/shell.rp6502", 0, NULL);
    ASSERT_FALSE(proc_has_launcher());
    proc_set_launcher(true);
    ASSERT_TRUE(proc_has_launcher());
    ASSERT_TRUE(proc_is_launcher());

    proc_set_argv("/game.rp6502", 0, NULL);
    ASSERT_FALSE(proc_is_launcher());
    ASSERT_TRUE(proc_has_launcher());

    /* proc_exit moves the machine to stopping but does not perform the stop,
     * so the shell's re-run is queued by proc_stop when sys_commit performs
     * it. */
    proc_exit(7);
    sys_commit();
    ASSERT_EQ(proc_get_exit_code(), 7);
    ASSERT_TRUE(proc_has_launcher());
    ASSERT_TRUE(proc_exec_inflight());
    proc_exec_init();

    proc_run();
    ASSERT_TRUE(proc_is_launcher());

    sys_run();
    sys_commit();
    proc_exit(0);
    sys_commit();
    ASSERT_FALSE(proc_has_launcher());
    ASSERT_FALSE(proc_exec_inflight());
}

UTEST(features, an_installed_name_round_trips_the_chain)
{
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));
    ASSERT_TRUE(rom_alias_insert(TEST_FIXTURE)); /* ":adventure.rp6502" */

    proc_set_argv(":adventure.rp6502", 0, NULL);
    proc_set_launcher(true);
    ASSERT_TRUE(proc_is_launcher());

    proc_set_argv("/game.rp6502", 0, NULL);
    proc_exit(0);
    sys_commit();
    ASSERT_TRUE(proc_exec_inflight());
    ASSERT_TRUE(proc_boot(":adventure.rp6502", 0, NULL, 0));
    sys_commit();
    ASSERT_STREQ(arg_index(0), ":adventure.rp6502");
}

UTEST(features, an_exec_is_not_the_child_exiting)
{
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));

    proc_set_argv("/shell.rp6502", 0, NULL);
    proc_set_launcher(true);
    proc_set_argv("/game.rp6502", 0, NULL);
    ASSERT_FALSE(proc_is_launcher());
    ASSERT_TRUE(proc_has_launcher());

    proc_set_argv("/other.rp6502", 0, NULL);
    proc_exec_request();
    ASSERT_TRUE(proc_exec_inflight());

    proc_exec_task();
    ASSERT_FALSE(proc_exec_inflight());
}

static float g_out[800 * 2];

static bool rendered_audio(int frames)
{
    for (int p = 0; p < frames; p++)
    {
        emu_frames(1);
        const int n = aud_render(g_out, 800);
        for (int i = 0; i < n * 2; i++)
            if (g_out[i] != 0.0f)
                return true;
    }
    return false;
}

UTEST(features, teletype_bell)
{
    sys_stop();
    sys_commit();

    ASSERT_TRUE(aud_enabled());
    ASSERT_TRUE(com_get_bel());

    com_set_bel(false);
    ASSERT_EQ(ssys_write(1, "\a", 1), 1);
    ASSERT_FALSE(rendered_audio(16));

    com_set_bel(true);
    ASSERT_EQ(ssys_write(1, "\a", 1), 1);
    ASSERT_TRUE(rendered_audio(16));
}

UTEST(features, audio_disable)
{
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));
    ASSERT_TRUE(aud_enabled());

    aud_set_enabled(false);
    ASSERT_FALSE(aud_enabled());

    bel_add(&bel_teletype);
    ASSERT_FALSE(rendered_audio(8));

    aud_set_enabled(true);
    emu_frames(60);
}

UTEST(features, zero_length_stdin_read_returns_at_once)
{
    sys_stop();
    sys_commit();
    char buf[1];
    ASSERT_EQ(ssys_read(0, buf, 0), 0);
    ASSERT_FALSE(std_stdin_waiting());
}

UTEST_MAIN_EMU()
