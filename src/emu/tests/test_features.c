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

#include "emu/api/pro.h"
#include "emu/aud/aud.h"
#include "emu/aud/snd.h"
#include "emu/mon/rom.h"
#include "emu/sys/ria.h"
#include "emu/sys/sys.h"
#include "aud/bel.h"
#include "sys/com.h"
#include "sys/ria.h"
#include "utest.h"
#include <stdio.h>
#include <string.h>

/* SIGINT: a Ctrl-C latches, is reported once via the attribute, and (only when
 * the program enabled the $FFF0 IRQ) asserts the CPU's IRQ line until read. */
UTEST(features, sigint_irq)
{
    ASSERT_TRUE(emu_rom_load(ADVENTURE_ROM));
    emu_init();

    ASSERT_FALSE(ria_irq_asserted()); /* idle at boot */

    /* The SIGINT attribute consumes the latch once. */
    com_kbd_push_byte(0x03);
    ASSERT_TRUE(ria_get_sigint());
    ASSERT_FALSE(ria_get_sigint());

    /* With the IRQ disabled, a pending SIGINT does not assert the line. */
    com_kbd_push_byte(0x03);
    ASSERT_FALSE(ria_irq_asserted());

    /* Writing the enable mask also acks the bits it names (firmware fallthrough),
     * so enabling does not immediately fire on the already-pending SIGINT. */
    ria_reg_write(0xFFF0, 0x40);
    ASSERT_FALSE(ria_irq_asserted());

    /* A fresh Ctrl-C now drives the IRQ line. */
    com_kbd_push_byte(0x03);
    ASSERT_TRUE(ria_irq_asserted());

    /* Reading $FFF0 returns the pending flags and acknowledges them. */
    uint8_t flags = ria_reg_read(0xFFF0);
    ASSERT_TRUE((flags & 0x40) != 0);
    ASSERT_FALSE(ria_irq_asserted());
}

/* Launcher: a shell registers itself, re-runs after each child exits, and the
 * chain ends when the shell itself exits. */
UTEST(features, launcher_chain)
{
    ASSERT_TRUE(emu_rom_load(ADVENTURE_ROM));
    emu_init();

    /* A shell starts and registers itself as the launcher. */
    pro_set_argv0("MSC0:/shell.rp6502");
    ASSERT_FALSE(pro_has_launcher());
    pro_set_launcher(true);
    ASSERT_TRUE(pro_has_launcher());
    ASSERT_TRUE(pro_is_launcher());

    /* It execs a game (the reload calls pro_run): the game is not the launcher. */
    pro_set_argv0("MSC0:/game.rp6502");
    ASSERT_FALSE(pro_is_launcher());
    ASSERT_TRUE(pro_has_launcher());

    /* The game exits -> the launcher is scheduled to re-run, chain still armed. */
    ASSERT_TRUE(pro_exit(7));
    ASSERT_EQ(pro_get_exit_code(), 7);
    ASSERT_TRUE(pro_has_launcher());

    /* The frame loop reloads the shell (pro_exit set its argv); pro_run picks it
     * up, so the shell is running again and is the launcher. */
    pro_run();
    ASSERT_TRUE(pro_is_launcher());

    /* The shell itself exits -> no relaunch, chain cleared. */
    ASSERT_FALSE(pro_exit(0));
    ASSERT_FALSE(pro_has_launcher());
}

/* Bell: the BEL is the standing audio device (firmware), present at boot and
 * silent until rung; ringing it makes the pump produce audible samples; the
 * enable flag round-trips. */
UTEST(features, teletype_bell)
{
    ASSERT_TRUE(emu_rom_load(ADVENTURE_ROM));
    emu_init();

    ASSERT_EQ(emu_audio_rate(), 24000); /* standing BEL device */
    ASSERT_TRUE(com_get_bel());         /* enabled by default */

    /* Ring the teletype bell (what a '\a' on console output does) and pump a few
     * frames: the standing handler now produces audible samples. */
    bel_add(&bel_teletype);
    static float buf[8192];
    bool nonzero = false;
    for (int f = 0; f < 16 && !nonzero; f++)
    {
        snd_task();
        int got = emu_audio_read(buf, 4096);
        for (int i = 0; i < got * 2; i++)
            if (buf[i] != 0.0f)
                nonzero = true;
    }
    ASSERT_TRUE(nonzero);

    com_set_bel(false);
    ASSERT_FALSE(com_get_bel());
}

/* --no-audio (emu_set_audio_enabled(false)): no rate is reported and the synth
 * generates no samples at all — not even for a rung bell. */
UTEST(features, audio_disable)
{
    ASSERT_TRUE(emu_rom_load(ADVENTURE_ROM));
    emu_init();
    ASSERT_EQ(emu_audio_rate(), 24000); /* enabled by default */

    emu_set_audio_enabled(false);
    ASSERT_FALSE(emu_audio_enabled());
    ASSERT_EQ(emu_audio_rate(), 0);

    /* Ringing a bell and pumping must produce nothing (no per-sample work). */
    bel_add(&bel_teletype);
    static float buf[4096];
    int total = 0;
    for (int f = 0; f < 8; f++)
    {
        snd_task();
        total += emu_audio_read(buf, 2048);
    }
    ASSERT_EQ(total, 0);

    emu_set_audio_enabled(true); /* restore the default for any later test */
}

/* Write a minimal .rp6502 carrying an "emulator" args asset, for the launch-arg
 * feature. The reset vector is the only program data (the test never runs the
 * CPU). Returns the path, or NULL on a write failure. */
static const char *make_asset_rom(const char *args)
{
    static const char *path = "/tmp/emu_asset_test.rp6502";
    uint8_t vec[2] = {0x00, 0x02}; /* reset vector -> $0200 */
    char rec[64];
    int reclen = snprintf(rec, sizeof rec, "$FFFC $2 $%08X\r\n", emu_crc32(0, vec, 2));
    FILE *f = fopen(path, "wb");
    if (!f)
        return NULL;
    fprintf(f, "#!RP6502\r\n");
    fprintf(f, "#>$%X $0\r\n", reclen + 2); /* program section = record line + 2 vector bytes */
    fwrite(rec, 1, (size_t)reclen, f);
    fwrite(vec, 1, 2, f);
    fprintf(f, "#>$%X $0 emulator\r\n", (unsigned)strlen(args));
    fwrite(args, 1, strlen(args), f);
    fclose(f);
    return path;
}

/* The launch ROM's "emulator" asset is readable by name (main.c parses it as
 * args). The tokenizer/parser + command-line precedence are exercised by the
 * binary's integration path; here we pin the asset read the loader registers. */
UTEST(features, rom_emulator_asset)
{
    const char *args = "--frames 50 --no-audio --input=\"hi there\"";
    const char *path = make_asset_rom(args);
    ASSERT_TRUE(path != NULL);
    ASSERT_TRUE(emu_rom_load(path));

    char buf[128];
    long n = fs_read_rom_asset("emulator", buf, sizeof buf);
    ASSERT_EQ(n, (long)strlen(args));
    buf[n] = 0;
    ASSERT_STREQ(buf, args);

    ASSERT_EQ(fs_read_rom_asset("nope", buf, sizeof buf), -1L); /* absent -> -1 */
    remove(path);
}

UTEST_MAIN()
