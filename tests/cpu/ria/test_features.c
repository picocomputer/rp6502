/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Direct (no-ROM-behavior) checks for the firmware-parity features the desktop
 * emulator grew: the $FFF0 SIGINT interrupt, the program launcher chain, and
 * the teletype bell. These drive the C interfaces straight rather than through
 * a 6502 program, so each contract is pinned without a bespoke test ROM.
 */

#include "core/sys/sys.h"
#include "core/sys/ria.h"
#include "core/api/arg.h"
#include "core/sys/proc.h"
#include "core/api/std.h"
#include "core/aud/mix.h"
#include "core/ria/regs.h"
#include "core/aud/bel.h"
#include "core/hid/vtkeys.h"
#include "core/com/com.h"
#include "core/ria/ria.h"
#include "stdsys.h"
#include "emu_boot.h"
#include <stdio.h>
#include <string.h>

/* SIGINT: a Ctrl-C latches, is reported once via the attribute, and (only when
 * the program enabled the $FFF0 IRQ) asserts the CPU's IRQ line until read. */
UTEST(features, sigint_irq)
{
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));

    ASSERT_FALSE(ria_irq_asserted()); /* idle at boot */

    /* The SIGINT attribute consumes the latch once. */
    vtkeys_ctrl_letter('c');
    ASSERT_TRUE(ria_get_sigint());
    ASSERT_FALSE(ria_get_sigint());

    /* With the IRQ disabled, a pending SIGINT does not assert the line. */
    vtkeys_ctrl_letter('c');
    ASSERT_FALSE(ria_irq_asserted());

    /* Writing the enable mask also acks the bits it names (firmware fallthrough),
     * so enabling does not immediately fire on the already-pending SIGINT. */
    ria_reg_write(0xFFF0, 0x40);
    ASSERT_FALSE(ria_irq_asserted());

    /* A fresh Ctrl-C now drives the IRQ line. */
    vtkeys_ctrl_letter('c');
    ASSERT_TRUE(ria_irq_asserted());

    /* Reading $FFF0 returns the pending flags and acknowledges them. */
    uint8_t flags = ria_reg_read(0xFFF0);
    ASSERT_TRUE((flags & 0x40) != 0);
    ASSERT_FALSE(ria_irq_asserted());
}

/* Reading $FFF0 acks the pending flags, but IRQB must still be asserted on the very
 * cycle that reads it — ria_tick samples the line before servicing the access. Every
 * other test drives ria_reg_read directly, so only a tick-level check can catch a
 * one-cycle-early deassert. */
UTEST(features, ria_tick_holds_irq_through_ack)
{
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));

    ria_reg_write(0xFFF0, 0x40); /* enable SIGINT (the write acks it too) */
    vtkeys_ctrl_letter('c');
    ASSERT_TRUE(ria_irq_asserted());

    uint8_t data = 0;
    ASSERT_TRUE(ria_tick(0xFFF0, true, &data)); /* still asserted on the acking cycle */
    ASSERT_TRUE((data & 0x40) != 0);            /* and the flags reached the data bus */

    ASSERT_FALSE(ria_tick(0x0000, true, &data)); /* deasserted the next cycle */
}

/* Launcher: a shell registers itself, re-runs after each child exits, and the
 * chain ends when the shell itself exits. */
UTEST(features, launcher_chain)
{
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));

    /* A shell starts and registers itself as the launcher. */
    proc_set_argv("MSC0:/shell.rp6502", 0, NULL);
    ASSERT_FALSE(proc_has_launcher());
    proc_set_launcher(true);
    ASSERT_TRUE(proc_has_launcher());
    ASSERT_TRUE(proc_is_launcher());

    /* It execs a game (the reload calls proc_run): the game is not the launcher. */
    proc_set_argv("MSC0:/game.rp6502", 0, NULL);
    ASSERT_FALSE(proc_is_launcher());
    ASSERT_TRUE(proc_has_launcher());

    /* The game exits. The stop walk decides the chain, so the re-run is armed
     * by the commit rather than by the exit itself. */
    proc_exit(7);
    sys_commit();
    ASSERT_EQ(proc_get_exit_code(), 7);
    ASSERT_TRUE(proc_has_launcher());
    ASSERT_TRUE(proc_exec_inflight()); /* the shell's re-run, queued once */
    proc_exec_init();                 /* standing in for the proc_exec_task that loads it */

    /* proc_run picks up the argv the chain left, so the shell is running
     * again and is the launcher. */
    proc_run();
    ASSERT_TRUE(proc_is_launcher());

    /* The shell itself exits -> no relaunch, chain cleared. */
    sys_run();
    sys_commit();
    proc_exit(0);
    sys_commit();
    ASSERT_FALSE(proc_has_launcher());
    ASSERT_FALSE(proc_exec_inflight());
}

/* An installed ROM's ":name" spelling survives the launcher chain verbatim:
 * argv[0] is recorded and replayed exactly, and the reload resolves it
 * through the alias map the same way the first exec did. */
UTEST(features, an_installed_name_round_trips_the_chain)
{
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));
    ASSERT_TRUE(rom_alias_insert(TEST_FIXTURE)); /* ":adventure.rp6502" */

    /* The launcher runs from the null drive and registers. */
    proc_set_argv(":adventure.rp6502", 0, NULL);
    proc_set_launcher(true);
    ASSERT_TRUE(proc_is_launcher());

    /* A child by filesystem path; the launcher's spelling is what replays. */
    proc_set_argv("MSC0:/game.rp6502", 0, NULL);
    proc_exit(0);
    sys_commit();
    ASSERT_TRUE(proc_exec_inflight());
    /* The re-run boots the ":name" itself -- the whole path through resolve,
     * the seam and the loader, not just the string. */
    ASSERT_TRUE(proc_boot(":adventure.rp6502", 0, NULL, 0));
    sys_commit();
    ASSERT_STREQ(arg_index(0), ":adventure.rp6502");
}

/* An exec is not an exit. proc_boot stops the machine on its way in, and that
 * stop runs the same walk a program's exit does -- so the chain must be able
 * to tell "this program is going away because it asked to be replaced" from
 * "this program ended, put the launcher back". Get it wrong and the launcher
 * loads over the child the program just asked for. */
UTEST(features, an_exec_is_not_the_child_exiting)
{
    ASSERT_TRUE(emu_restart(TEST_FIXTURE)); /* running, which the stop needs */

    proc_set_argv("MSC0:/shell.rp6502", 0, NULL);
    proc_set_launcher(true);
    proc_set_argv("MSC0:/game.rp6502", 0, NULL);
    ASSERT_FALSE(proc_is_launcher());
    ASSERT_TRUE(proc_has_launcher());

    proc_set_argv("MSC0:/other.rp6502", 0, NULL);
    proc_exec_request(); /* op 0x09, machine still running */
    ASSERT_TRUE(proc_exec_inflight());

    /* Performing it leaves nothing queued behind. The image cannot load here,
     * which is fine: the queue is cleared before the load either way. */
    proc_exec_task();
    ASSERT_FALSE(proc_exec_inflight());
}

/* Empty args are protocol elements: the seeded argv keeps them, so the
 * emulator and the monitor's LOAD deliver the same argc. Read back through
 * the RIA_OP_ARGV blob (offset table + {0,0} + packed strings). */
UTEST(features, empty_args_kept)
{
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));

    char *args[] = {"", "x", ""};
    ASSERT_TRUE(proc_set_argv("MSC0:/a.rp6502", 3, args));
    ASSERT_FALSE(proc_api_argv()); /* false = op complete, not still working */

    const uint8_t *blob = &xstack[xstack_ptr];
    int argc = 0;
    while (blob[argc * 2] || blob[argc * 2 + 1])
        argc++;
    ASSERT_EQ(argc, 4);
    const char *argv1 = (const char *)&blob[blob[2] | (blob[3] << 8)];
    const char *argv2 = (const char *)&blob[blob[4] | (blob[5] << 8)];
    const char *argv3 = (const char *)&blob[blob[6] | (blob[7] << 8)];
    ASSERT_STREQ(argv1, "");
    ASSERT_STREQ(argv2, "x");
    ASSERT_STREQ(argv3, "");
}

/* Run a frame and take it as the device would, until the machine makes a
 * nonzero sample or the budget runs out. Only what the machine made
 * counts: a short render repeats a level, which is not evidence. */
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

/* Bell: the BEL is the standing audio device (firmware), present at boot and
 * silent until rung. A BEL (0x07) in a program's console output rings the
 * teletype bell, and the enable flag gates that ring end to end. */
UTEST(features, teletype_bell)
{
    /* No program: the writes below are dispatched from here, and a running
     * program's own syscall would be in flight between them. */
    sys_stop();
    sys_commit();

    ASSERT_TRUE(aud_enabled());
    ASSERT_TRUE(com_get_bel());         /* enabled by default */

    /* Disabled (nothing has rung yet): a BEL byte is ignored and stays silent. */
    com_set_bel(false);
    ASSERT_EQ(ssys_write(1, "\a", 1), 1); /* fd 1 = stdout */
    ASSERT_FALSE(rendered_audio(16));

    /* Enabled: the same BEL byte now rings the bell -> audible samples. */
    com_set_bel(true);
    ASSERT_EQ(ssys_write(1, "\a", 1), 1);
    ASSERT_TRUE(rendered_audio(16));
}

/* --mute (aud_set_enabled(false)): the synth generates no samples at all —
 * not even for a rung bell. */
UTEST(features, audio_disable)
{
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));
    ASSERT_TRUE(aud_enabled()); /* enabled by default */

    aud_set_enabled(false);
    ASSERT_FALSE(aud_enabled());

    /* A rung bell renders as silence: the handler never runs. */
    bel_add(&bel_teletype);
    ASSERT_FALSE(rendered_audio(8));

    aud_set_enabled(true); /* restore the default for any later test */
    /* Play out the bell we rang: audio is a continuous stream (a reset never
     * silences it), so let it end here instead of bleeding into a later test. */
    emu_frames(60);
}

UTEST_MAIN_EMU()
