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
#include "core/ria/regs.h"
#include "core/aud/bel.h"
#include "core/hid/vtkeys.h"
#include "core/com/com.h"
#include "core/ria/ria.h"
#include "core/sys/xram.h"
#include "stdsys.h"
#include "emu_boot.h"
#include <stdio.h>
#include <string.h>

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

UTEST(features, empty_args_kept)
{
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));

    char *args[] = {"", "x", ""};
    ASSERT_TRUE(proc_set_argv("/a.rp6502", 3, args));
    ASSERT_FALSE(proc_api_argv());

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

/* exec.rp6502 starts with argc 2 here, so it writes 2 and argv[1] from $0001
 * and leaves $0000 alone, which it writes only before executing itself again.
 * PROC_UNCHAIN is needed because earlier cases leave a launcher registered
 * that does not exist on disk, and relaunching it after this program exits
 * would fail and set the exit code to 1. */
UTEST(features, boot_args_reach_program)
{
    xram_set_fill(false, 0, 0);
    char *args[] = {"Foo"};
    ASSERT_TRUE(proc_boot(EXEC_ROM, 1, args, PROC_REFILL | PROC_UNCHAIN));
    sys_commit();
    emu_frames(20);

    static const uint8_t want[] = {0, 2, 'F', 'o', 'o', 0};
    ASSERT_EQ(memcmp((const uint8_t *)xram, want, sizeof want), 0);
    ASSERT_EQ(proc_get_exit_code(), 0);
    ASSERT_FALSE(sys_running());
}

/* A stop only marks the canvas for reset, and vga_task performs the reset
 * during the next frame. */
UTEST(features, stop_resets_canvas_to_console)
{
    ASSERT_TRUE(emu_restart(ROMS_DIR "/mode3_1bpp.rp6502"));
    emu_frames(20);
    ASSERT_EQ(vga_get_canvas(), vga_canvas_320_240);

    sys_stop();
    sys_commit();
    emu_frames(1);
    ASSERT_EQ(vga_get_canvas(), vga_canvas_console);
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

static char tap_term[64];
static size_t tap_term_len;
static char tap_std[2][64];
static size_t tap_std_len[2];

static void term_tap(const char *buf, int len)
{
    for (int i = 0; i < len && tap_term_len < sizeof tap_term - 1; i++)
        tap_term[tap_term_len++] = buf[i];
    tap_term[tap_term_len] = 0;
}

static void std_tap(int fd, const char *buf, int len)
{
    char *dst = tap_std[fd == 2];
    size_t *n = &tap_std_len[fd == 2];
    for (int i = 0; i < len && *n < sizeof tap_std[0] - 1; i++)
        dst[(*n)++] = buf[i];
    dst[*n] = 0;
}

static void taps_reset(void)
{
    tap_term_len = tap_std_len[0] = tap_std_len[1] = 0;
    tap_term[0] = tap_std[0][0] = tap_std[1][0] = 0;
}

UTEST(features, stderr_is_its_own_stream)
{
    sys_stop();
    sys_commit();
    com_set_tx_tap(term_tap);
    com_set_std_tap(std_tap);

    taps_reset();
    ASSERT_EQ(ssys_write(2, "err\n", 4), 4);
    ASSERT_STREQ(tap_std[1], "err\n");
    ASSERT_STREQ(tap_std[0], "");
    ASSERT_STREQ(tap_term, "err\r\n");

    taps_reset();
    ASSERT_EQ(ssys_write(1, "out\n", 4), 4);
    ASSERT_STREQ(tap_std[0], "out\n");
    ASSERT_STREQ(tap_std[1], "");
    ASSERT_STREQ(tap_term, "out\r\n");

    taps_reset();
    ria_reg_write(0xFFE1, '\n');
    ASSERT_STREQ(tap_std[0], "\n");
    ASSERT_STREQ(tap_term, "\n");

    com_set_bel(true);
    ASSERT_EQ(ssys_write(2, "\a", 1), 1);
    ASSERT_TRUE(rendered_audio(16));

    com_set_tx_tap(NULL);
    com_set_std_tap(NULL);
}

UTEST(features, zero_length_stdin_read_returns_at_once)
{
    sys_stop();
    sys_commit();
    char buf[1];
    ASSERT_EQ(ssys_read(0, buf, 0), 0);
    ASSERT_FALSE(std_stdin_waiting());
}

UTEST(features, stdin_eof_ends_a_pending_read)
{
    sys_stop();
    sys_commit();
    uint16_t n = 8;
    xstack_ptr = XSTACK_SIZE - 2;
    memcpy(&xstack[xstack_ptr], &n, 2);
    API_A = 0;
    ASSERT_TRUE(std_api_read_xstack());
    ASSERT_FALSE(std_stdin_waiting());
    ASSERT_TRUE(std_api_read_xstack());
    ASSERT_TRUE(std_stdin_waiting());
    std_stdin_eof();
    ASSERT_FALSE(std_stdin_waiting());
    ASSERT_FALSE(std_api_read_xstack());
    ASSERT_EQ(dsys_ax(), 0);
    xstack_ptr = XSTACK_SIZE;

    char buf[8];
    ASSERT_EQ(ssys_read(0, buf, 8), 0);
    ASSERT_FALSE(std_stdin_waiting());
}

UTEST_MAIN_EMU()
