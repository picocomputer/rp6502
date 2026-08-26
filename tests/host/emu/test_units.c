/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Unit tests for the pure-logic corners: CRC-32, the .rp6502 loader, the
 * xreg device/channel dispatch, and the CLI parser.
 */

#include "core/api/oem.h"
#include "core/str/str.h"
#include "host/sokol/cli.h"
#include "core/hid/usage.h"
#include "core/sys/keyboard.h"
#include "core/hid/gamepad.h"
#include "core/hid/tablet.h"
#include "core/sys/main.h"
#include "core/sys/rom.h"
#include "core/mem/mem.h"
#include "core/com/com.h"
#include "utest.h"
#include <stdio.h>
#include <string.h>

UTEST(crc32, known_vectors)
{
    /* CRC-32/ISO-HDLC (zlib) check value for "123456789". */
    ASSERT_EQ(mem_crc32(0, "123456789", 9), (uint32_t)0xCBF43926u);
    ASSERT_EQ(mem_crc32(0, "", 0), (uint32_t)0x00000000u);
}

UTEST(rom, loads)
{
    memset(ram, 0, 0x10000);
    ASSERT_TRUE(rom_load(TEST_FIXTURE));
    /* the loader places code at the $0200 entry and points the reset vector
     * there ($FFFC/$FFFD -> $0200). */
    ASSERT_EQ(ram[0xFFFC], 0x00);
    ASSERT_EQ(ram[0xFFFD], 0x02);
    ASSERT_NE(ram[0x0200], 0x00);
}

UTEST(rom, rejects_missing_file)
{
    ASSERT_FALSE(rom_load("/nonexistent/definitely-not-a.rp6502"));
}

/* The headerless form: the magic line and then bare records, with no #>
 * directory and no chunk length to bound them. Nothing writes it any more —
 * the generators went to the tool's format when it turned out theirs made
 * images the monitor would not load — so the loader's fallback is claimed
 * here rather than left to whichever fixture happened to still be in it. */
UTEST(rom, loads_a_headerless_image)
{
    static const char image[] =
        "#!RP6502\n"
        "$00300 $4 $06EE5D17\n" "\xA9\x2A\xDB\xEA"
        "$0FFFC $2 $D8D04345\n" "\x00\x03";
    char path[512];
    snprintf(path, sizeof path, "%s/headerless.rp6502", TEST_SCRATCH);
    FILE *f = fopen(path, "wb");
    ASSERT_TRUE(f != NULL);
    ASSERT_EQ(fwrite(image, 1, sizeof image - 1, f), sizeof image - 1);
    fclose(f);

    memset(ram, 0, 0x10000);
    ASSERT_TRUE(rom_load(path));
    ASSERT_EQ(ram[0x0300], 0xA9);
    ASSERT_EQ(ram[0x0302], 0xDB);
    ASSERT_EQ(ram[0xFFFC], 0x00);
    ASSERT_EQ(ram[0xFFFD], 0x03);
}

UTEST(xreg, device_channel_dispatch)
{
    ASSERT_TRUE(main_xreg_0(0, 0, 0)); /* RIA-local devices: accepted (stub) */
    ASSERT_TRUE(main_xreg_1(0, 0, 3)); /* VGA canvas 640x480 */
    /* The control channel: CODE_PAGE is answered, and so is DISPLAY, which is
     * not exercised here because it resets the machine. The rest are registers
     * of a real VGA chip that a machine which is its own has no analog for. */
    ASSERT_TRUE(main_xreg_1(15, 1, 437));
    ASSERT_FALSE(main_xreg_1(15, 2, 0));
    ASSERT_TRUE(main_xreg_1(5, 0, 0)); /* VGA channel 1-14: over the bus, no ACK, AX=0 */
}

/* The host gamepad bridge (web Gamepad API path): mapping gate + the report
 * encoding that mirrors the firmware (status bits, analog->digital sticks byte,
 * L2/R2 trigger<->button coupling). */
UTEST(gamepad, host_report_encoding)
{
    gamepad_stop();
    ASSERT_FALSE(gamepad_is_mapped()); /* nothing touches input until a ROM maps it */

    ASSERT_TRUE(main_xreg_0(0, 2, 0xFF00)); /* xreg_ria_gamepad(0xFF00) */
    ASSERT_TRUE(gamepad_is_mapped());
    ASSERT_EQ(xram[0xFF00], 0x00); /* published default: player 0 disconnected */

    /* Player 0: dpad up + A, left stick full north, host sure of nothing. */
    gamepad_connect(0, true, GAMEPAD_TYPE_UNKNOWN, false);
    gamepad_host_report(0, 0x01, 0x01, 0x00, 0, -127, 0, 0, 0, 0);
    ASSERT_EQ(xram[0xFF00 + 0], 0x81);          /* dpad up | connected */
    ASSERT_EQ(xram[0xFF00 + 1], 0x01);          /* sticks: left=N, right=center */
    ASSERT_EQ(xram[0xFF00 + 2], 0x01);          /* button0: A */
    ASSERT_EQ(xram[0xFF00 + 3], 0x00);          /* button1 */
    ASSERT_EQ(xram[0xFF00 + 5], (uint8_t)-127); /* ly passthrough */

    /* Type and sticks are claims about the controller, made when it is
     * plugged in, and they land in their own bits. */
    gamepad_connect(1, true, GAMEPAD_TYPE_PLAYSTATION, true);
    ASSERT_EQ(xram[0xFF00 + 10], 0xF0); /* connected | sticks | playstation */
    gamepad_connect(1, true, GAMEPAD_TYPE_EASTERN, false);
    ASSERT_EQ(xram[0xFF00 + 10], 0xA0); /* connected | eastern, no sticks */
    gamepad_connect(1, true, GAMEPAD_TYPE_WESTERN, true);
    ASSERT_EQ(xram[0xFF00 + 10], 0xD0); /* connected | sticks | western */

    /* L2 button with no analog reads full-scale; analog past deadzone asserts
     * the button — both couplings, like the firmware. */
    gamepad_connect(2, true, GAMEPAD_TYPE_UNKNOWN, false);
    gamepad_host_report(2, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0);
    ASSERT_EQ(xram[0xFF00 + 20 + 8], 255);  /* lt forced to full */
    ASSERT_EQ(xram[0xFF00 + 20 + 3], 0x01); /* button1 keeps L2 */
    gamepad_connect(3, true, GAMEPAD_TYPE_UNKNOWN, false);
    gamepad_host_report(3, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 200);
    ASSERT_EQ(xram[0xFF00 + 30 + 3], 0x02); /* rt>deadzone asserts R2 */

    /* Unplug blanks the record; unmapping clears the gate. */
    gamepad_connect(0, false, GAMEPAD_TYPE_UNKNOWN, false);
    ASSERT_EQ(xram[0xFF00 + 0], 0x00);
    ASSERT_TRUE(main_xreg_0(0, 2, 0xFFFF));
    ASSERT_FALSE(gamepad_is_mapped());
}

/* The tablet's mouse-format wheel/pan: header bytes +2/+3 are 8-bit wrapping
 * accumulators fed by host scroll, exactly like the mouse block. */
UTEST(tablet, host_wheel_encoding)
{
    tablet_stop();
    ASSERT_FALSE(tablet_is_mapped()); /* nothing touches input until a ROM maps it */

    ASSERT_TRUE(main_xreg_0(0, 3, 0xFF00)); /* xreg_ria_tablet(0xFF00) */
    ASSERT_TRUE(tablet_is_mapped());
    ASSERT_EQ(xram[0xFF00 + 2], 0x00); /* wheel default 0 */
    ASSERT_EQ(xram[0xFF00 + 3], 0x00); /* pan default 0 */

    tablet_host_wheel(3, -2);
    ASSERT_EQ(xram[0xFF00 + 2], (uint8_t)3);  /* wheel accumulates */
    ASSERT_EQ(xram[0xFF00 + 3], (uint8_t)-2); /* pan accumulates (wraps) */

    tablet_host_wheel(-4, 5);
    ASSERT_EQ(xram[0xFF00 + 2], (uint8_t)-1); /* 3 + (-4) wraps */
    ASSERT_EQ(xram[0xFF00 + 3], (uint8_t)3);  /* -2 + 5 */

    ASSERT_TRUE(main_xreg_0(0, 3, 0xFFFF));
    ASSERT_FALSE(tablet_is_mapped());
}

/* Drain the keyboard com ring (what keyboard_key/keyboard_text push) into buf. */
static int keyboard_drain(char *buf, int max)
{
    int n = 0, c;
    com_source_t src = COM_SOURCE_KEYBOARD;
    while (n < max && (c = com_getchar(&src)) >= 0)
    {
        buf[n++] = (char)c;
        src = COM_SOURCE_KEYBOARD;
    }
    return n;
}

/* The special-key ANSI the firmware (and xterm) emit, including the
 * ESC[1;{mod} modifier annotations. */
UTEST(keyboard, ansi_sequences)
{
    char b[32];

    com_init();
    keyboard_key(HID_KEY_ARROW_UP, false, false, false);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 3);
    ASSERT_EQ(0, memcmp(b, "\33[A", 3)); /* CSI arrow */

    com_init();
    keyboard_key(HID_KEY_F1, false, false, false);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 3);
    ASSERT_EQ(0, memcmp(b, "\33OP", 3)); /* SS3 for F1-F4 */

    com_init();
    keyboard_key(HID_KEY_F5, false, false, false);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 5);
    ASSERT_EQ(0, memcmp(b, "\33[15~", 5)); /* VT220 numbered */

    com_init();
    keyboard_key(HID_KEY_F12, false, false, false);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 5);
    ASSERT_EQ(0, memcmp(b, "\33[24~", 5));

    com_init();
    keyboard_key(HID_KEY_INSERT, false, false, false);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 4);
    ASSERT_EQ(0, memcmp(b, "\33[2~", 4));

    com_init();
    keyboard_key(HID_KEY_HOME, false, false, false);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 3);
    ASSERT_EQ(0, memcmp(b, "\33[H", 3));

    /* Modifier annotations: 1 + shift + alt*2 + ctrl*4. */
    com_init();
    keyboard_key(HID_KEY_ARROW_UP, true, false, false); /* ctrl -> 5 */
    ASSERT_EQ(keyboard_drain(b, sizeof b), 6);
    ASSERT_EQ(0, memcmp(b, "\33[1;5A", 6));

    com_init();
    keyboard_key(HID_KEY_F1, false, true, false); /* shift -> 2 */
    ASSERT_EQ(keyboard_drain(b, sizeof b), 6);
    ASSERT_EQ(0, memcmp(b, "\33[1;2P", 6));

    com_init();
    keyboard_key(HID_KEY_END, false, true, true); /* shift+alt -> 4 */
    ASSERT_EQ(keyboard_drain(b, sizeof b), 6);
    ASSERT_EQ(0, memcmp(b, "\33[1;4F", 6));

    com_init();
    keyboard_key(HID_KEY_PAGE_UP, true, false, false); /* ctrl -> 5 */
    ASSERT_EQ(keyboard_drain(b, sizeof b), 6);
    ASSERT_EQ(0, memcmp(b, "\33[5;5~", 6));

    /* Editing keys: CR for Enter, DEL (0x7f) for plain backspace, BS (0x08) with ctrl. */
    com_init();
    keyboard_key(HID_KEY_ENTER, false, false, false);
    keyboard_key(HID_KEY_BACKSPACE, false, false, false);
    keyboard_key(HID_KEY_BACKSPACE, true, false, false);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 3);
    ASSERT_EQ(0, memcmp(b, "\r\x7f\x08", 3));
}

/* Ctrl and Alt on the four keys that spell a character of their own. The
 * console keymap defines no control form for Enter, Tab or Escape -- each is
 * already a C0 control -- so the key still types itself, while Alt is an ESC
 * prefix over whatever the other modifiers settled on. */
UTEST(keyboard, ctrl_and_alt_on_control_keys)
{
    char b[16];

    com_init();
    keyboard_key(HID_KEY_ENTER, true, false, false);
    keyboard_key(HID_KEY_TAB, true, false, false);
    keyboard_key(HID_KEY_ESCAPE, true, false, false);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 3);
    ASSERT_EQ(0, memcmp(b, "\r\t\x1b", 3));

    com_init();
    keyboard_key(HID_KEY_ENTER, false, false, true);
    keyboard_key(HID_KEY_TAB, false, false, true);
    keyboard_key(HID_KEY_ESCAPE, false, false, true);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 6);
    ASSERT_EQ(0, memcmp(b, "\x1b\r\x1b\t\x1b\x1b", 6));

    /* Alt composes with Ctrl instead of replacing it: ESC, then the byte
     * Ctrl already chose. */
    com_init();
    keyboard_key(HID_KEY_BACKSPACE, false, false, true);
    keyboard_key(HID_KEY_BACKSPACE, true, false, true);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 4);
    ASSERT_EQ(0, memcmp(b, "\x1b\x7f\x1b\x08", 4));
}

/* Typed text is converted UTF-8 -> active OEM code page (default 437). */
UTEST(keyboard, text_to_oem)
{
    char b[32];
    str_init(); /* apply the default locale: code page 437 */

    com_init();
    keyboard_text("Hi!"); /* ASCII passes through */
    ASSERT_EQ(keyboard_drain(b, sizeof b), 3);
    ASSERT_EQ(0, memcmp(b, "Hi!", 3));

    com_init();
    keyboard_text("\xC3\xA9"); /* U+00E9 'é' -> cp437 0x82 */
    ASSERT_EQ(keyboard_drain(b, sizeof b), 1);
    ASSERT_EQ((unsigned char)b[0], 0x82u);

    com_init();
    keyboard_text("\xF0\x9F\x98\x80"); /* U+1F600 unmappable -> 0x7F */
    ASSERT_EQ(keyboard_drain(b, sizeof b), 1);
    ASSERT_EQ((unsigned char)b[0], 0x7Fu);
}

/* The oem string family: UTF-8 <-> OEM round-trip in the active code page,
 * snprintf-style overflow reporting, and the counted wide entry (the USB
 * string descriptor shape). */
UTEST(oem, utf8_string_roundtrip)
{
    str_init(); /* apply the default locale: code page 437 */

    char oem[16], u8[16];
    ASSERT_EQ(oem_from_utf8("caf\xC3\xA9", oem, sizeof oem), (size_t)4);
    ASSERT_STREQ(oem, "caf\x82"); /* CP437 'é' */
    ASSERT_EQ(oem_to_utf8(oem, u8, sizeof u8), (size_t)5);
    ASSERT_STREQ(u8, "caf\xC3\xA9");

    /* unmappable codepoint and malformed lead byte -> 0x7F */
    ASSERT_EQ(oem_from_utf8("\xF0\x9F\x98\x80", oem, sizeof oem), (size_t)1);
    ASSERT_EQ((unsigned char)oem[0], 0x7Fu);
    ASSERT_EQ(oem_from_utf8("\xFF", oem, sizeof oem), (size_t)1);
    ASSERT_EQ((unsigned char)oem[0], 0x7Fu);

    /* overlong forms too: 0xC0 0xAF must not decode to '/' */
    ASSERT_EQ(oem_from_utf8("A\xC0\xAF", oem, sizeof oem), (size_t)2);
    ASSERT_EQ(oem[0], 'A');
    ASSERT_EQ((unsigned char)oem[1], 0x7Fu);

    /* snprintf-style: the return is the untruncated length, and a sequence
     * never splits — a 2-byte dst can't hold 'é' (2 UTF-8 bytes) plus the
     * NUL, so none of it is written. */
    ASSERT_EQ(oem_to_utf8("\x82", u8, 2), (size_t)2);
    ASSERT_EQ(u8[0], 0);
    ASSERT_EQ(oem_from_utf8("caf\xC3\xA9", oem, 3), (size_t)4);
    ASSERT_STREQ(oem, "ca");

    /* counted UTF-16 (USB descriptors are not NUL-terminated) */
    uint16_t w[3] = {'a', 0x00E9, 0x2603}; /* 'a' 'é' snowman */
    ASSERT_EQ(oem_from_wide_n(w, 3, oem, sizeof oem), (size_t)3);
    ASSERT_EQ(oem[0], 'a');
    ASSERT_EQ((unsigned char)oem[1], 0x82u);
    ASSERT_EQ((unsigned char)oem[2], 0x7Fu);

    /* the page drives the mapping: 'ã' is CP850 0xC6, absent from CP437 */
    oem_set_code_page_run(850);
    ASSERT_EQ(oem_from_utf8("\xC3\xA3", oem, sizeof oem), (size_t)1);
    ASSERT_EQ((unsigned char)oem[0], 0xC6u);
    str_init(); /* back to the default 437 for later tests */
}

/* Everything after "--" is the ROM's argv[1..], never parsed as options. */
UTEST(cli, rom_args_after_separator)
{
    cli_options o;
    cli_options_init(&o);
    char *argv[] = {"emu", "rom.rp6502", "--", "--looks-like-an-option", "b"};
    ASSERT_EQ(cli_parse_args(5, argv, &o), 0);
    ASSERT_STREQ(o.rom, "rom.rp6502");
    ASSERT_EQ(o.n_rom_args, 2);
    ASSERT_STREQ(o.rom_args[0], "--looks-like-an-option");
    ASSERT_STREQ(o.rom_args[1], "b");
}

UTEST(cli, rom_args_with_install_form)
{
    cli_options o;
    cli_options_init(&o);
    char *argv[] = {"emu", "--rom", "x.rp6502", "--", "a"};
    ASSERT_EQ(cli_parse_args(5, argv, &o), 0);
    ASSERT_EQ(o.n_installs, 1);
    ASSERT_TRUE(o.rom == NULL);
    ASSERT_EQ(o.n_rom_args, 1);
    ASSERT_STREQ(o.rom_args[0], "a");
}

/* A bare "--" is presence (rom_args non-NULL, zero words): a later pass can
 * override an asset preset with "no args". */
UTEST(cli, rom_args_bare_separator_and_passes)
{
    cli_options o;
    cli_options_init(&o);
    char *asset[] = {"emulator", "--mute", "--", "x"};
    ASSERT_EQ(cli_parse_args(4, asset, &o), 0);
    ASSERT_EQ(o.n_rom_args, 1);
    ASSERT_STREQ(o.rom_args[0], "x");

    char *cli[] = {"emu", "rom.rp6502", "--"};
    ASSERT_EQ(cli_parse_args(3, cli, &o), 0);
    ASSERT_TRUE(o.rom_args != NULL);
    ASSERT_EQ(o.n_rom_args, 0);
    ASSERT_STREQ(o.rom, "rom.rp6502");

    char *plain[] = {"emu", "--mute"};
    ASSERT_EQ(cli_parse_args(2, plain, &o), 0); /* no "--": earlier pass stands */
    ASSERT_TRUE(o.rom_args != NULL);
    ASSERT_EQ(o.n_rom_args, 0);
}

UTEST(cli, no_separator_no_rom_args)
{
    cli_options o;
    cli_options_init(&o);
    char *argv[] = {"emu", "rom.rp6502"};
    ASSERT_EQ(cli_parse_args(2, argv, &o), 0);
    ASSERT_TRUE(o.rom_args == NULL);
    ASSERT_EQ(o.n_rom_args, 0);
    ASSERT_STREQ(o.rom, "rom.rp6502");
}

UTEST_MAIN();
