/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Unit tests for the pure-logic corners: CRC-32, the .rp6502 loader, the
 * xreg device/channel dispatch, and the CLI parser.
 */

#include "emu/api/oem.h"
#include "emu/app/cli.h"
#include "emu/hid/kbd.h"
#include "emu/hid/pad.h"
#include "emu/mon/rom.h"
#include "emu/sys/mem.h"
#include "emu/sys/xreg.h"
#include "sys/com.h"
#include "utest.h"
#include <string.h>

UTEST(crc32, known_vectors)
{
    /* CRC-32/ISO-HDLC (zlib) check value for "123456789". */
    ASSERT_EQ(emu_crc32(0, "123456789", 9), (uint32_t)0xCBF43926u);
    ASSERT_EQ(emu_crc32(0, "", 0), (uint32_t)0x00000000u);
}

UTEST(rom, loads)
{
    memset(ram, 0, 0x10000);
    ASSERT_TRUE(emu_rom_load(ADVENTURE_ROM));
    /* the loader places code at the $0200 entry and points the reset vector
     * there ($FFFC/$FFFD -> $0200). */
    ASSERT_EQ(ram[0xFFFC], 0x00);
    ASSERT_EQ(ram[0xFFFD], 0x02);
    ASSERT_NE(ram[0x0200], 0x00);
}

UTEST(rom, rejects_missing_file)
{
    ASSERT_FALSE(emu_rom_load("/nonexistent/definitely-not-a.rp6502"));
}

UTEST(xreg, device_channel_dispatch)
{
    ASSERT_TRUE(xreg_write(0, 0, 0, 0));  /* RIA-local devices: accepted (stub) */
    ASSERT_TRUE(xreg_write(1, 0, 0, 3));  /* VGA canvas 640x480 */
    ASSERT_FALSE(xreg_write(1, 15, 0, 0)); /* VGA control channel is RIA-private (EACCES) */
    ASSERT_TRUE(xreg_write(2, 0, 0, 0));  /* PIX device 2-7: over the bus, no ACK, AX=0 (firmware parity) */
    ASSERT_TRUE(xreg_write(1, 5, 0, 0));  /* VGA channel 1-14: over the bus, no ACK, AX=0 */
}

/* The host gamepad bridge (web Gamepad API path): mapping gate + the report
 * encoding that mirrors the firmware (connected/sony bits, analog->digital
 * sticks byte, L2/R2 trigger<->button coupling). */
UTEST(pad, host_report_encoding)
{
    pad_reset();
    ASSERT_FALSE(pad_is_mapped()); /* nothing touches input until a ROM maps it */

    ASSERT_TRUE(xreg_write(0, 0, 2, 0xFF00)); /* xreg_ria_gamepad(0xFF00) */
    ASSERT_TRUE(pad_is_mapped());
    ASSERT_EQ(xram[0xFF00], 0x00); /* published default: player 0 disconnected */

    /* Player 0: dpad up + A, left stick full north. */
    pad_host_report(0, 0x01, 0x01, 0x00, 0, -127, 0, 0, 0, 0, false);
    ASSERT_EQ(xram[0xFF00 + 0], 0x81);       /* dpad up | connected */
    ASSERT_EQ(xram[0xFF00 + 1], 0x01);        /* sticks: left=N, right=center */
    ASSERT_EQ(xram[0xFF00 + 2], 0x01);        /* button0: A */
    ASSERT_EQ(xram[0xFF00 + 3], 0x00);        /* button1 */
    ASSERT_EQ(xram[0xFF00 + 5], (uint8_t)-127); /* ly passthrough */

    /* Sony feature bit lands in the dpad byte. */
    pad_host_report(1, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 0, true);
    ASSERT_EQ(xram[0xFF00 + 10], 0xC0); /* connected | sony */

    /* L2 button with no analog reads full-scale; analog past deadzone asserts
     * the button — both couplings, like the firmware. */
    pad_host_report(2, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0, false);
    ASSERT_EQ(xram[0xFF00 + 20 + 8], 255);   /* lt forced to full */
    ASSERT_EQ(xram[0xFF00 + 20 + 3], 0x01);  /* button1 keeps L2 */
    pad_host_report(3, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 200, false);
    ASSERT_EQ(xram[0xFF00 + 30 + 3], 0x02);  /* rt>deadzone asserts R2 */

    /* Unplug blanks the record; unmapping clears the gate. */
    pad_connect(0, false);
    ASSERT_EQ(xram[0xFF00 + 0], 0x00);
    ASSERT_TRUE(xreg_write(0, 0, 2, 0xFFFF));
    ASSERT_FALSE(pad_is_mapped());
}

/* Drain the keyboard com ring (what kbd_key/kbd_text push) into buf. */
static int kbd_drain(char *buf, int max)
{
    int n = 0, c;
    com_source_t src = COM_SOURCE_KBD;
    while (n < max && (c = com_getchar(&src)) >= 0)
    {
        buf[n++] = (char)c;
        src = COM_SOURCE_KBD;
    }
    return n;
}

/* The special-key ANSI the firmware (and xterm) emit, including the
 * ESC[1;{mod} modifier annotations. */
UTEST(kbd, ansi_sequences)
{
    char b[32];

    com_reset();
    kbd_key(KBD_KEY_UP, false, false, false);
    ASSERT_EQ(kbd_drain(b, sizeof b), 3);
    ASSERT_EQ(0, memcmp(b, "\33[A", 3)); /* CSI arrow */

    com_reset();
    kbd_key(KBD_KEY_F1, false, false, false);
    ASSERT_EQ(kbd_drain(b, sizeof b), 3);
    ASSERT_EQ(0, memcmp(b, "\33OP", 3)); /* SS3 for F1-F4 */

    com_reset();
    kbd_key(KBD_KEY_F5, false, false, false);
    ASSERT_EQ(kbd_drain(b, sizeof b), 5);
    ASSERT_EQ(0, memcmp(b, "\33[15~", 5)); /* VT220 numbered */

    com_reset();
    kbd_key(KBD_KEY_F12, false, false, false);
    ASSERT_EQ(kbd_drain(b, sizeof b), 5);
    ASSERT_EQ(0, memcmp(b, "\33[24~", 5));

    com_reset();
    kbd_key(KBD_KEY_INSERT, false, false, false);
    ASSERT_EQ(kbd_drain(b, sizeof b), 4);
    ASSERT_EQ(0, memcmp(b, "\33[2~", 4));

    com_reset();
    kbd_key(KBD_KEY_HOME, false, false, false);
    ASSERT_EQ(kbd_drain(b, sizeof b), 3);
    ASSERT_EQ(0, memcmp(b, "\33[H", 3));

    /* Modifier annotations: 1 + shift + alt*2 + ctrl*4. */
    com_reset();
    kbd_key(KBD_KEY_UP, true, false, false); /* ctrl -> 5 */
    ASSERT_EQ(kbd_drain(b, sizeof b), 6);
    ASSERT_EQ(0, memcmp(b, "\33[1;5A", 6));

    com_reset();
    kbd_key(KBD_KEY_F1, false, true, false); /* shift -> 2 */
    ASSERT_EQ(kbd_drain(b, sizeof b), 6);
    ASSERT_EQ(0, memcmp(b, "\33[1;2P", 6));

    com_reset();
    kbd_key(KBD_KEY_END, false, true, true); /* shift+alt -> 4 */
    ASSERT_EQ(kbd_drain(b, sizeof b), 6);
    ASSERT_EQ(0, memcmp(b, "\33[1;4F", 6));

    com_reset();
    kbd_key(KBD_KEY_PAGE_UP, true, false, false); /* ctrl -> 5 */
    ASSERT_EQ(kbd_drain(b, sizeof b), 6);
    ASSERT_EQ(0, memcmp(b, "\33[5;5~", 6));

    /* Editing keys: CR for Enter, DEL (0x7f) for plain backspace, BS (0x08) with ctrl. */
    com_reset();
    kbd_key(KBD_KEY_ENTER, false, false, false);
    kbd_key(KBD_KEY_BACKSPACE, false, false, false);
    kbd_key(KBD_KEY_BACKSPACE, true, false, false);
    ASSERT_EQ(kbd_drain(b, sizeof b), 3);
    ASSERT_EQ(0, memcmp(b, "\r\x7f\x08", 3));
}

/* Typed text is converted UTF-8 -> active OEM code page (default 437). */
UTEST(kbd, text_to_oem)
{
    char b[32];
    oem_reset(); /* code page 437 */

    com_reset();
    kbd_text("Hi!"); /* ASCII passes through */
    ASSERT_EQ(kbd_drain(b, sizeof b), 3);
    ASSERT_EQ(0, memcmp(b, "Hi!", 3));

    com_reset();
    kbd_text("\xC3\xA9"); /* U+00E9 'é' -> cp437 0x82 */
    ASSERT_EQ(kbd_drain(b, sizeof b), 1);
    ASSERT_EQ((unsigned char)b[0], 0x82u);

    com_reset();
    kbd_text("\xF0\x9F\x98\x80"); /* U+1F600 unmappable -> 0x7F */
    ASSERT_EQ(kbd_drain(b, sizeof b), 1);
    ASSERT_EQ((unsigned char)b[0], 0x7Fu);
}

/* Everything after "--" is the ROM's argv[1..], never parsed as options. */
UTEST(cli, rom_args_after_separator)
{
    options o;
    options_init(&o);
    char *argv[] = {"emu", "rom.rp6502", "--", "--looks-like-an-option", "b"};
    ASSERT_EQ(parse_args(5, argv, &o), 0);
    ASSERT_STREQ(o.rom, "rom.rp6502");
    ASSERT_EQ(o.n_rom_args, 2);
    ASSERT_STREQ(o.rom_args[0], "--looks-like-an-option");
    ASSERT_STREQ(o.rom_args[1], "b");
}

UTEST(cli, rom_args_with_install_form)
{
    options o;
    options_init(&o);
    char *argv[] = {"emu", "--rom", "x.rp6502", "--", "a"};
    ASSERT_EQ(parse_args(5, argv, &o), 0);
    ASSERT_EQ(o.n_installs, 1);
    ASSERT_TRUE(o.rom == NULL);
    ASSERT_EQ(o.n_rom_args, 1);
    ASSERT_STREQ(o.rom_args[0], "a");
}

/* A bare "--" is presence (rom_args non-NULL, zero words): a later pass can
 * override an asset preset with "no args". */
UTEST(cli, rom_args_bare_separator_and_passes)
{
    options o;
    options_init(&o);
    char *asset[] = {"emulator", "--mute", "--", "x"};
    ASSERT_EQ(parse_args(4, asset, &o), 0);
    ASSERT_EQ(o.n_rom_args, 1);
    ASSERT_STREQ(o.rom_args[0], "x");

    char *cli[] = {"emu", "rom.rp6502", "--"};
    ASSERT_EQ(parse_args(3, cli, &o), 0);
    ASSERT_TRUE(o.rom_args != NULL);
    ASSERT_EQ(o.n_rom_args, 0);
    ASSERT_STREQ(o.rom, "rom.rp6502");

    char *plain[] = {"emu", "--mute"};
    ASSERT_EQ(parse_args(2, plain, &o), 0); /* no "--": earlier pass stands */
    ASSERT_TRUE(o.rom_args != NULL);
    ASSERT_EQ(o.n_rom_args, 0);
}

UTEST(cli, no_separator_no_rom_args)
{
    options o;
    options_init(&o);
    char *argv[] = {"emu", "rom.rp6502"};
    ASSERT_EQ(parse_args(2, argv, &o), 0);
    ASSERT_TRUE(o.rom_args == NULL);
    ASSERT_EQ(o.n_rom_args, 0);
    ASSERT_STREQ(o.rom, "rom.rp6502");
}

UTEST_MAIN();
