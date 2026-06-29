/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Gamepad + OPL2 integration test, driven by loderunner.rp6502 (Doug Smith's
 * Lode Runner). The title screen waits for START and is otherwise static, so a
 * frame-CRC change after a gamepad press is an unambiguous "the 6502 saw the
 * input". This exercises the HID gamepad xreg (device 0, channel 0, address 2),
 * the 10-byte-per-player pad_xram_t mirror in pad.c, and the OPL2 (YM3812) xreg
 * + emu8950 core + snd.c pump that the game's music driver uses.
 */

#include "emu/aud/aud.h"
#include "emu/hid/pad.h"
#include "emu/mon/rom.h"
#include "emu/sys/mem.h"
#include "emu/sys/sys.h"
#include "emu/sys/vga.h"
#include "utest.h"

static uint32_t fb[EMU_FB_WIDTH * EMU_FB_HEIGHT];

static uint32_t frame_crc(void)
{
    int cw, ch;
    emu_render(fb);
    emu_canvas_size(&cw, &ch);
    return emu_crc32(0, fb, (size_t)cw * ch * 4);
}

static void run(int n)
{
    for (int i = 0; i < n; i++)
        emu_run_frame();
}

/* Drain and sum the energy of all audio generated so far this run. */
static double drain_audio_energy(void)
{
    static float buf[4096 * 2];
    double e = 0;
    int got;
    while ((got = emu_audio_read(buf, 4096)) > 0)
        for (int s = 0; s < got * 2; s++)
            e += (double)buf[s] * buf[s];
    return e;
}

/* The xreg maps a four-player block (10 bytes each) into XRAM and keeps it in
 * sync. Byte 0 carries the dpad (0x0F) plus the connected feature bit (0x80);
 * bytes 2/3 are button0/button1. No ROM needed — this pokes pad.c directly. */
UTEST(gamepad, xram_mirror)
{
    pad_reset();

    /* The block must fit below 0x10000; 40 bytes won't fit above 0xFFD8. */
    ASSERT_FALSE(pad_set_xram(0xFFD9));
    ASSERT_TRUE(pad_set_xram(0xFFD8));
    ASSERT_TRUE(pad_set_xram(0xFF78)); /* the address Lode Runner uses */

    /* Unplugged: the whole 10-byte record reads as zero (no connected bit). */
    for (int i = 0; i < 10; i++)
        ASSERT_EQ(xram[0xFF78 + i], 0);

    pad_connect(0, true);
    ASSERT_EQ(xram[0xFF78 + 0] & 0x80, 0x80);

    pad_hid_set(0, PAD_BTN_DPAD_LEFT, true);
    ASSERT_EQ(xram[0xFF78 + 0] & 0x0F, 0x04); /* dpad-left bit */
    pad_hid_set(0, PAD_BTN_A, true);
    ASSERT_EQ(xram[0xFF78 + 2] & 0x01, 0x01); /* button0: A */
    pad_hid_set(0, PAD_BTN_START, true);
    ASSERT_EQ(xram[0xFF78 + 3] & 0x08, 0x08); /* button1: Start */

    pad_hid_set(0, PAD_BTN_DPAD_LEFT, false);
    ASSERT_EQ(xram[0xFF78 + 0] & 0x0F, 0x00); /* released, connected bit stays */
    ASSERT_EQ(xram[0xFF78 + 0] & 0x80, 0x80);

    /* Player 1 lives at +10 and is independent of player 0. */
    pad_connect(1, true);
    ASSERT_EQ(xram[0xFF78 + 10] & 0x80, 0x80);
    ASSERT_EQ(xram[0xFF78 + 10] & 0x0F, 0x00);

    /* Unplugging blanks the whole record. */
    pad_connect(0, false);
    for (int i = 0; i < 10; i++)
        ASSERT_EQ(xram[0xFF78 + i], 0);

    /* pad_reset unmaps the block: later input must not touch XRAM. */
    xram[0xFF78] = 0xAB;
    pad_reset();
    pad_connect(0, true);
    ASSERT_EQ(xram[0xFF78], 0xAB);
}

/* The music driver brings up the OPL2 (YM3812) at its fixed 49716 Hz and
 * streams register writes; the emu8950 core must produce a sustained signal. */
UTEST(loderunner, opl2_music_plays)
{
    ASSERT_TRUE(emu_rom_load(LODERUNNER_ROM));
    emu_init();

    double energy = 0;
    for (int i = 0; i < 120; i++)
    {
        emu_run_frame();
        energy += drain_audio_energy();
    }
    ASSERT_EQ(emu_audio_rate(), 49716); /* OPL2 engaged via xreg(0,1,1,..) */
    ASSERT_GT(energy, 1.0);             /* music is actually playing */
    ASSERT_FALSE(emu_cpu_halted);
}

/* The title waits for START. A gamepad whose connected bit (dpad 0x80) is clear
 * must be ignored, exactly as the firmware gates on it: pressing START on an
 * unplugged pad leaves the (static) title frame unchanged. */
UTEST(loderunner, disconnected_pad_ignored)
{
    ASSERT_TRUE(emu_rom_load(LODERUNNER_ROM));
    emu_init();
    run(240); /* settle on the title screen */
    uint32_t title = frame_crc();
    run(20);
    ASSERT_EQ(frame_crc(), title); /* title is static frame-to-frame */

    /* No pad_connect: the press rides an unplugged controller. */
    pad_hid_set(0, PAD_BTN_START, true);
    run(20);
    pad_hid_set(0, PAD_BTN_START, false);
    run(40);
    ASSERT_EQ(frame_crc(), title); /* still on the title; START was ignored */
}

/* A connected gamepad's START leaves the title, and in-game the dpad moves the
 * runner — each direction redraws the frame. Covers the whole pad_xram_t mirror
 * end to end: the connected byte, the button1 START bit, and the dpad bits. */
UTEST(loderunner, gamepad_drives_game)
{
    ASSERT_TRUE(emu_rom_load(LODERUNNER_ROM));
    emu_init();
    run(240);
    uint32_t title = frame_crc();

    pad_connect(0, true);
    pad_hid_set(0, PAD_BTN_START, true);
    run(20);
    pad_hid_set(0, PAD_BTN_START, false);
    run(60);
    uint32_t playing = frame_crc();
    ASSERT_NE(playing, title); /* START (connected) entered the level */

    pad_hid_set(0, PAD_BTN_DPAD_RIGHT, true);
    run(40);
    pad_hid_set(0, PAD_BTN_DPAD_RIGHT, false);
    run(5);
    uint32_t moved_right = frame_crc();
    ASSERT_NE(moved_right, playing); /* the runner moved right */

    pad_hid_set(0, PAD_BTN_DPAD_LEFT, true);
    run(40);
    pad_hid_set(0, PAD_BTN_DPAD_LEFT, false);
    run(5);
    ASSERT_NE(frame_crc(), moved_right); /* and back left */

    ASSERT_FALSE(emu_cpu_halted);
}

UTEST_MAIN()
