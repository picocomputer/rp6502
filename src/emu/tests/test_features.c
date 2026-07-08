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
#include "emu/api/std.h"
#include "emu/aud/aud.h"
#include "emu/mon/rom.h"
#include "emu/chips/rp6502.h"
#include "emu/sys/mem.h"
#include "emu/main.h"
#include "aud/bel.h"
#include "sys/com.h"
#include "sys/ria.h"
#include "stdsys.h"
#include "utest.h"
#include <stdio.h>
#include <string.h>

/* SIGINT: a Ctrl-C latches, is reported once via the attribute, and (only when
 * the program enabled the $FFF0 IRQ) asserts the CPU's IRQ line until read. */
UTEST(features, sigint_irq)
{
    ASSERT_TRUE(rom_load(ADVENTURE_ROM));
    main_init();

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
    ASSERT_TRUE(rom_load(ADVENTURE_ROM));
    main_init();

    /* A shell starts and registers itself as the launcher. */
    pro_set_argv("MSC0:/shell.rp6502", 0, NULL);
    ASSERT_FALSE(pro_has_launcher());
    pro_set_launcher(true);
    ASSERT_TRUE(pro_has_launcher());
    ASSERT_TRUE(pro_is_launcher());

    /* It execs a game (the reload calls pro_run): the game is not the launcher. */
    pro_set_argv("MSC0:/game.rp6502", 0, NULL);
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

/* Empty args are protocol elements: the seeded argv keeps them, so the
 * emulator and the monitor's LOAD deliver the same argc. Read back through
 * the RIA_OP_ARGV blob (offset table + {0,0} + packed strings). */
UTEST(features, empty_args_kept)
{
    ASSERT_TRUE(rom_load(ADVENTURE_ROM));
    main_init();

    char *args[] = {"", "x", ""};
    ASSERT_TRUE(pro_set_argv("MSC0:/a.rp6502", 3, args));
    ASSERT_FALSE(pro_api_argv()); /* false = op complete, not still working */

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

/* Pump frames, draining audio, until a nonzero sample appears or the budget
 * runs out. Returns whether the standing handler produced any audible output. */
static bool pumped_audio(int frames)
{
    static float buf[8192];
    for (int f = 0; f < frames; f++)
    {
        aud_task();
        int got = aud_read(buf, 4096);
        for (int i = 0; i < got * 2; i++)
            if (buf[i] != 0.0f)
                return true;
    }
    return false;
}

/* Bell: the BEL is the standing audio device (firmware), present at boot and
 * silent until rung. A BEL (0x07) in a program's console output rings the
 * teletype bell, and the enable flag gates that ring end to end. */
UTEST(features, teletype_bell)
{
    ASSERT_TRUE(rom_load(ADVENTURE_ROM));
    main_init();

    ASSERT_EQ(aud_rate(), 24000); /* standing BEL device */
    ASSERT_TRUE(com_get_bel());         /* enabled by default */

    /* Disabled (nothing has rung yet): a BEL byte is ignored and stays silent. */
    com_set_bel(false);
    ASSERT_EQ(ssys_write(1, "\a", 1), 1); /* fd 1 = stdout */
    ASSERT_FALSE(pumped_audio(16));

    /* Enabled: the same BEL byte now rings the bell -> audible samples. */
    com_set_bel(true);
    ASSERT_EQ(ssys_write(1, "\a", 1), 1);
    ASSERT_TRUE(pumped_audio(16));
}

/* --mute (aud_set_enabled(false)): no rate is reported and the synth
 * generates no samples at all — not even for a rung bell. */
UTEST(features, audio_disable)
{
    ASSERT_TRUE(rom_load(ADVENTURE_ROM));
    main_init();
    ASSERT_EQ(aud_rate(), 24000); /* enabled by default */

    aud_set_enabled(false);
    ASSERT_FALSE(aud_enabled());
    ASSERT_EQ(aud_rate(), 0);

    /* Ringing a bell and pumping must produce nothing (no per-sample work). */
    bel_add(&bel_teletype);
    static float buf[4096];
    int total = 0;
    for (int f = 0; f < 8; f++)
    {
        aud_task();
        total += aud_read(buf, 2048);
    }
    ASSERT_EQ(total, 0);

    aud_set_enabled(true); /* restore the default for any later test */
    /* Drain the bell we rang: audio is a continuous stream (a reset never
     * silences it), so play it out here instead of bleeding into a later test. */
    for (int f = 0; f < 128; f++)
    {
        aud_task();
        aud_read(buf, 2048);
    }
}

/* Write a minimal .rp6502 carrying an "emulator" args asset, for the launch-arg
 * feature. The reset vector is the only program data (the test never runs the
 * CPU). Returns the path, or NULL on a write failure. */
static const char *make_asset_rom(const char *args)
{
    static const char *path = "/tmp/emu_asset_test.rp6502";
    uint8_t vec[2] = {0x00, 0x02}; /* reset vector -> $0200 */
    char rec[64];
    int reclen = snprintf(rec, sizeof rec, "$FFFC $2 $%08X\r\n", rom_crc32(0, vec, 2));
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
    const char *args = "--frames 50 --mute --input=\"hi there\"";
    const char *path = make_asset_rom(args);
    ASSERT_TRUE(path != NULL);
    ASSERT_TRUE(rom_load(path));

    char buf[128];
    long n = rom_read_asset("emulator", buf, sizeof buf);
    ASSERT_EQ(n, (long)strlen(args));
    buf[n] = 0;
    ASSERT_STREQ(buf, args);

    ASSERT_EQ(rom_read_asset("nope", buf, sizeof buf), -1L); /* absent -> -1 */
    remove(path);
}

UTEST_MAIN()
