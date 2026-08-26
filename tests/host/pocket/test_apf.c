/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The Pocket's synthetic HID descriptors, against the drivers they were
 * written for.
 *
 * src/rtl/sw/apf.c hands the dock's registers to ria/hid as reports,
 * and what makes that work is four descriptors that say where every
 * bit went. Nothing else checks them. A descriptor that put the d-pad
 * where the buttons go, or the right stick where the left one is, would
 * build clean, boot clean, and be wrong in a way only a hand on a
 * controller could find — so the registers go in here and the XRAM
 * records the drivers publish are what is read back.
 *
 * apf.c is included rather than linked: its report builder is static,
 * because a report of a register is not an interface, and this is the
 * one caller that is not apf_task. Its MMIO macros are addresses that
 * are never dereferenced here, because nothing below calls apf_task.
 */

#include "core/api/oem.h"
#include "core/hid/kbd.h"
#include "core/hid/kbt.h"
#include "core/hid/mou.h"
#include "core/hid/pad.h"
#include "core/hid/tab.h"
#include "core/mem.h"

#include "host/pocket/sw/apf.c"

#include "utest.h"

#include <string.h>

/* Where each driver publishes. Far enough apart that an overrun by one
 * would land in another's record rather than in nothing. */
#define AT_PAD 0x1000
#define AT_KBD 0x2000
#define AT_MOU 0x3000

/* pad_xram_t, which apf.c's descriptor exists to fill correctly. */
#define PAD_DPAD 0
#define PAD_STICKS 1
#define PAD_BUTTON0 2
#define PAD_BUTTON1 3
#define PAD_LX 4
#define PAD_LY 5
#define PAD_RX 6
#define PAD_RY 7
#define PAD_LT 8
#define PAD_RT 9

static const uint8_t *pad_rec(int player)
{
    return (uint8_t *)&xram[AT_PAD + player * 10];
}

/* One slot's registers, exactly as apf_slot_task would put them through
 * — the same builder, the same drivers, and the same pacing, because a
 * level that has not moved is not sent again and that is a rule with
 * consequences. Returns the report's length so a caller can say a slot
 * built one at all. */
static uint8_t feed(int slot, uint8_t type, uint32_t key, uint32_t joy,
                    uint16_t trig, bool first)
{
    uint8_t report[APF_REPORT_MAX];
    uint8_t len = apf_build(type, key, joy, trig, first, report);
    if (len && (type == APF_TYPE_MOU || apf_changed(slot, report, len)))
        apf_report(slot, report, len);
    return len;
}

static void reset_all(void)
{
    for (int slot = 0; slot < MMIO_CONT_SLOTS; slot++)
        apf_umount(slot);
    apf_refresh();
    kbd_init();
    mou_init();
    pad_init();
    tab_init();
    /* Whatever the last case typed is still queued — kbd_init does not
     * empty a queue a console would have drained. */
    char drain[64];
    while (kbt_in_chars(drain, sizeof drain))
        ;
    memset((uint8_t *)xram, 0, 0x10000);
    kbd_xreg(AT_KBD);
    mou_xreg(AT_MOU);
    pad_xreg(AT_PAD);
}

/* The whole point of the change: every control goes where its own name
 * says. The d-pad is a d-pad and not the left stick, the sticks are the
 * sticks in APF's order, and the triggers are the triggers. */
UTEST(apf, a_pad_maps_straight_through)
{
    reset_all();
    apf_mount(0, APF_TYPE_PAD_ANA);

    /* Nothing pressed, sticks centered: connected, and still. */
    feed(0, APF_TYPE_PAD_ANA, 0x30000000u, 0x80808080u, 0x0000, false);
    ASSERT_EQ(pad_rec(0)[PAD_DPAD] & 0x80, 0x80);
    ASSERT_EQ(pad_rec(0)[PAD_DPAD] & 0x0F, 0);
    ASSERT_EQ(pad_rec(0)[PAD_BUTTON0], 0);
    ASSERT_EQ(pad_rec(0)[PAD_BUTTON1], 0);

    /* The four directions, one at a time, in APF's bit order. The XRAM
     * record's own order is up, down, left, right — the same. */
    static const struct
    {
        uint32_t key;
        uint8_t dpad;
    } dirs[] = {
        {1u << 0, 0x1}, {1u << 1, 0x2}, {1u << 2, 0x4}, {1u << 3, 0x8},
    };
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++)
    {
        feed(0, APF_TYPE_PAD_ANA, 0x30000000u | dirs[i].key, 0x80808080u,
             0x0000, false);
        ASSERT_EQ(pad_rec(0)[PAD_DPAD] & 0x0F, dirs[i].dpad);
        /* And the d-pad is not the left stick, which is the bug this
         * whole descriptor replaced. */
        ASSERT_EQ((int8_t)pad_rec(0)[PAD_LX], 0);
        ASSERT_EQ((int8_t)pad_rec(0)[PAD_LY], 0);
        ASSERT_EQ(pad_rec(0)[PAD_STICKS], 0);
    }

    /* Every button, at the index pad_xram_t names for it. */
    static const struct
    {
        uint32_t key;
        int byte;
        uint8_t bit;
    } buttons[] = {
        {1u << 4, PAD_BUTTON0, 1u << 0},  /* A      -> index 0 */
        {1u << 5, PAD_BUTTON0, 1u << 1},  /* B      -> index 1 */
        {1u << 6, PAD_BUTTON0, 1u << 3},  /* X      -> index 3 */
        {1u << 7, PAD_BUTTON0, 1u << 4},  /* Y      -> index 4 */
        {1u << 8, PAD_BUTTON0, 1u << 6},  /* L1     -> index 6 */
        {1u << 9, PAD_BUTTON0, 1u << 7},  /* R1     -> index 7 */
        {1u << 14, PAD_BUTTON1, 1u << 2}, /* select -> index 10 */
        {1u << 15, PAD_BUTTON1, 1u << 3}, /* start  -> index 11 */
        {1u << 12, PAD_BUTTON1, 1u << 5}, /* L3     -> index 13 */
        {1u << 13, PAD_BUTTON1, 1u << 6}, /* R3     -> index 14 */
    };
    for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; i++)
    {
        feed(0, APF_TYPE_PAD_ANA, 0x30000000u | buttons[i].key, 0x80808080u,
             0x0000, false);
        int other = buttons[i].byte == PAD_BUTTON0 ? PAD_BUTTON1 : PAD_BUTTON0;
        ASSERT_EQ(pad_rec(0)[buttons[i].byte], buttons[i].bit);
        ASSERT_EQ(pad_rec(0)[other], 0);
        ASSERT_EQ(pad_rec(0)[PAD_DPAD] & 0x0F, 0);
    }

    /* L2 and R2 are buttons that pad.c also reads as triggers fully
     * down, which is how a digital pad gets analog ones. */
    feed(0, APF_TYPE_PAD_ANA, 0x30000000u | (1u << 10), 0x80808080u, 0x0000,
         false);
    ASSERT_EQ(pad_rec(0)[PAD_BUTTON1] & 0x01, 0x01);
    ASSERT_EQ(pad_rec(0)[PAD_LT], 255);
    ASSERT_EQ(pad_rec(0)[PAD_RT], 0);
    feed(0, APF_TYPE_PAD_ANA, 0x30000000u | (1u << 11), 0x80808080u, 0x0000,
         false);
    ASSERT_EQ(pad_rec(0)[PAD_BUTTON1] & 0x02, 0x02);
    ASSERT_EQ(pad_rec(0)[PAD_RT], 255);

    /* The sticks, in APF's word order: left X, left Y, right X, right Y,
     * each unsigned with 0x80 at rest. Four different values, because a
     * swapped pair is what this is looking for. */
    feed(0, APF_TYPE_PAD_ANA, 0x30000000u, 0x40C000FFu, 0x0000, false);
    ASSERT_EQ((int8_t)pad_rec(0)[PAD_LX], 127);  /* joy[7:0]   = 0xFF */
    ASSERT_EQ((int8_t)pad_rec(0)[PAD_LY], -128); /* joy[15:8]  = 0x00 */
    ASSERT_EQ((int8_t)pad_rec(0)[PAD_RX], 64);   /* joy[23:16] = 0xC0 */
    ASSERT_EQ((int8_t)pad_rec(0)[PAD_RY], -64);  /* joy[31:24] = 0x40 */

    /* The triggers, analog, in APF's order: left low, right high. */
    feed(0, APF_TYPE_PAD_ANA, 0x30000000u, 0x80808080u, 0x40C0, false);
    ASSERT_EQ(pad_rec(0)[PAD_LT], 0xC0);
    ASSERT_EQ(pad_rec(0)[PAD_RT], 0x40);
}

/* A pad with no analog to send still has a d-pad and buttons, and its
 * sticks read centered rather than reading whatever the unused words
 * happened to hold. */
UTEST(apf, a_pad_without_analog_is_centered)
{
    reset_all();
    apf_mount(1, APF_TYPE_PAD);
    feed(1, APF_TYPE_PAD, 0x20000000u | (1u << 2), 0xDEADBEEFu, 0xFFFF, false);
    ASSERT_EQ(pad_rec(0)[PAD_DPAD] & 0x0F, 0x4); /* left */
    ASSERT_EQ((int8_t)pad_rec(0)[PAD_LX], 0);
    ASSERT_EQ((int8_t)pad_rec(0)[PAD_LY], 0);
    ASSERT_EQ((int8_t)pad_rec(0)[PAD_RX], 0);
    ASSERT_EQ((int8_t)pad_rec(0)[PAD_RY], 0);
    ASSERT_EQ(pad_rec(0)[PAD_LT], 0);
    ASSERT_EQ(pad_rec(0)[PAD_RT], 0);
}

/* The status nibble is the descriptor's other job. A pad whose descriptor
 * declares both sticks says so; one whose descriptor stops after the buttons
 * must not, and must still be recognized as a gamepad at all despite having
 * no axes to creak with. The Pocket's own buttons are the only labels APF
 * ever tells us about. */
UTEST(apf, the_status_bits_follow_the_descriptor)
{
    reset_all();

    apf_mount(0, APF_TYPE_PAD_ANA);
    feed(0, APF_TYPE_PAD_ANA, 0x30000000u, 0x80808080u, 0x0000, false);
    ASSERT_EQ(pad_rec(0)[PAD_DPAD] & 0xF0, 0xC0); /* connected | sticks */

    reset_all();
    apf_mount(0, APF_TYPE_PAD);
    feed(0, APF_TYPE_PAD, 0x20000000u, 0x80808080u, 0x0000, false);
    ASSERT_EQ(pad_rec(0)[PAD_DPAD] & 0xF0, 0x80); /* connected, nothing claimed */

    reset_all();
    apf_mount(0, APF_TYPE_POCKET);
    feed(0, APF_TYPE_POCKET, 0x10000000u, 0x80808080u, 0x0000, false);
    ASSERT_EQ(pad_rec(0)[PAD_DPAD] & 0xF0, 0x80 | (PAD_TYPE_EASTERN << 4));

    /* And the labels are a claim about labels only: A is still index 0. */
    feed(0, APF_TYPE_POCKET, 0x10000000u | (1u << 4), 0x80808080u, 0x0000, false);
    ASSERT_EQ(pad_rec(0)[PAD_BUTTON0], 1u << 0);
}

/* Four slots holding four pads are four players, in slot order. */
UTEST(apf, four_pads_are_four_players)
{
    reset_all();
    for (int slot = 0; slot < 4; slot++)
        apf_mount(slot, APF_TYPE_PAD_ANA);
    for (int slot = 0; slot < 4; slot++)
        feed(slot, APF_TYPE_PAD_ANA, 0x30000000u | (1u << (4 + slot)),
             0x80808080u, 0x0000, false);
    /* A, B, X and Y are indices 0, 1, 3 and 4. */
    static const uint8_t want[4] = {1u << 0, 1u << 1, 1u << 3, 1u << 4};
    for (int player = 0; player < 4; player++)
    {
        ASSERT_EQ(pad_rec(player)[PAD_DPAD] & 0x80, 0x80);
        ASSERT_EQ(pad_rec(player)[PAD_BUTTON0], want[player]);
    }
}

/* The keyboard: six scan codes and the modifier byte, into the bitmap a
 * program reads. */
UTEST(apf, a_keyboard_sets_the_bitmap)
{
    reset_all();
    apf_mount(2, APF_TYPE_KBD);

    /* Idle: bit 0 of word 0 says no keys are down. */
    feed(2, APF_TYPE_KBD, 0x40000000u, 0, 0, false);
    ASSERT_EQ(xram[AT_KBD] & 1, 1);

    /* 'a' is 0x04, and APF packs the first four codes into joy, MSB
     * first, with the last two in trig. */
    feed(2, APF_TYPE_KBD, 0x40000000u, 0x04000000u, 0x0000, false);
    ASSERT_EQ(xram[AT_KBD + (0x04 >> 3)] & (1 << (0x04 & 7)), 1 << (0x04 & 7));
    ASSERT_EQ(xram[AT_KBD] & 1, 0);

    /* All six at once, and the modifiers, which are keycodes 0xE0 and
     * up. Left shift is bit 1 of the HID modifier byte — and that byte
     * is the modifier field's first, so it arrives in [15:8]. */
    feed(2, APF_TYPE_KBD, 0x40000200u, 0x04050607u, 0x0809, false);
    for (uint8_t kc = 0x04; kc <= 0x09; kc++)
        ASSERT_EQ(xram[AT_KBD + (kc >> 3)] & (1 << (kc & 7)), 1 << (kc & 7));
    ASSERT_EQ(xram[AT_KBD + (0xE1 >> 3)] & (1 << (0xE1 & 7)), 1 << (0xE1 & 7));

    /* Released, all of them: the report is the whole state. */
    feed(2, APF_TYPE_KBD, 0x40000000u, 0, 0, false);
    ASSERT_EQ(xram[AT_KBD] & 1, 1);
    ASSERT_EQ(xram[AT_KBD + (0x04 >> 3)] & (1 << (0x04 & 7)), 0);
}

/* A layout is what turns a scan code into a character, and the console
 * is where it goes. This is the path that did not exist before. */
UTEST(apf, a_keyboard_types_through_its_layout)
{
    reset_all();
    kbt_load_layout("US");
    apf_mount(2, APF_TYPE_KBD);

    char buf[8];
    feed(2, APF_TYPE_KBD, 0x40000000u, 0x04000000u, 0x0000, false); /* a */
    ASSERT_EQ(kbt_in_chars(buf, sizeof buf), (size_t)1);
    ASSERT_EQ(buf[0], 'a');

    /* Shift is the modifier byte's bit 1, in [15:8], and the layout's
     * second column. Reading it from [7:0] is a keyboard that types
     * but never shifts, which is what hardware showed. */
    feed(2, APF_TYPE_KBD, 0x40000000u, 0, 0, false);
    feed(2, APF_TYPE_KBD, 0x40000200u, 0x04000000u, 0x0000, false);
    ASSERT_EQ(kbt_in_chars(buf, sizeof buf), (size_t)1);
    ASSERT_EQ(buf[0], 'A');

    /* And a layout that moves a key moves it: on the German layout the
     * key at 0x1C types z, not y. */
    kbt_load_layout("DE");
    feed(2, APF_TYPE_KBD, 0x40000000u, 0, 0, false);
    feed(2, APF_TYPE_KBD, 0x40000000u, 0x1C000000u, 0x0000, false);
    ASSERT_EQ(kbt_in_chars(buf, sizeof buf), (size_t)1);
    ASSERT_EQ(buf[0], 'z');
}

/* A dead key composes against a table of OEM bytes, so that table is only good
 * for the code page it was built from. A 6502 program can change the page while
 * a layout is loaded, and nothing tells the keyboard -- on this machine the page
 * is the font's, and the font does not know a keyboard exists. So the keyboard
 * has to notice for itself.
 *
 * The discriminator is a character one page has and the other does not: a with
 * tilde is 0xC6 in CP850 and absent from CP437. */
UTEST(apf, dead_keys_follow_a_code_page_change)
{
    reset_all();
    oem_set_code_page_run(437);
    kbt_load_layout("US-INTL");
    apf_mount(2, APF_TYPE_KBD);

    char buf[8];

    /* Shift+0x35 is the tilde, the dead key. CP437 cannot spell what it
     * composes to, so the pair does not compose -- an unmatched dead key
     * types both of its characters. Whatever it did, it was not 0xC6. */
    feed(2, APF_TYPE_KBD, 0x40000200u, 0x35000000u, 0x0000, false);
    feed(2, APF_TYPE_KBD, 0x40000000u, 0, 0, false);
    feed(2, APF_TYPE_KBD, 0x40000000u, 0x04000000u, 0x0000, false); /* a */
    size_t n437 = kbt_in_chars(buf, sizeof buf);
    for (size_t i = 0; i < n437; i++)
        ASSERT_NE((unsigned char)buf[i], 0xC6);
    while (kbt_in_chars(buf, sizeof buf)) /* nothing of 437 left queued */
        ;

    /* The same two keys, after the page moves under the layout. */
    oem_set_code_page_run(850);
    feed(2, APF_TYPE_KBD, 0x40000000u, 0, 0, false);
    feed(2, APF_TYPE_KBD, 0x40000200u, 0x35000000u, 0x0000, false);
    feed(2, APF_TYPE_KBD, 0x40000000u, 0, 0, false);
    feed(2, APF_TYPE_KBD, 0x40000000u, 0x04000000u, 0x0000, false); /* a */
    ASSERT_EQ(kbt_in_chars(buf, sizeof buf), (size_t)1);
    ASSERT_EQ((unsigned char)buf[0], 0xC6);
}

/* The mouse: buttons and two signed movements, and the report counter
 * that says a report is new. */
UTEST(apf, a_mouse_moves_and_clicks)
{
    reset_all();
    apf_mount(3, APF_TYPE_MOU);

    /* The first report after it appears carries no movement, however
     * far the hand went before anyone was listening. The movements are
     * byte swapped in the register, so +0x40 arrives as 0x4000. */
    feed(3, APF_TYPE_MOU, 0x50000001u, 0x00014000u, 0x4000, true);
    uint8_t x0 = xram[AT_MOU + 1];
    uint8_t y0 = xram[AT_MOU + 2];
    ASSERT_EQ(xram[AT_MOU + 0], 0x01); /* joy[23:16] is the buttons */

    /* Then movement accumulates, published at half resolution. */
    feed(3, APF_TYPE_MOU, 0x50000200u, 0x00004000u, 0x2000, false);
    ASSERT_EQ(xram[AT_MOU + 1], (uint8_t)(x0 + 0x20));
    ASSERT_EQ(xram[AT_MOU + 2], (uint8_t)(y0 + 0x10));
    ASSERT_EQ(xram[AT_MOU + 0], 0x00);

    /* Negative deltas go the other way, which is what the descriptor's
     * signed sixteen-bit fields are for. -0x40 is 0xFFC0, and swapped
     * that is 0xC0FF. */
    feed(3, APF_TYPE_MOU, 0x50000300u, 0x0000C0FFu, 0xE0FF, false);
    ASSERT_EQ(xram[AT_MOU + 1], (uint8_t)(x0 + 0x20 - 0x20));
    ASSERT_EQ(xram[AT_MOU + 2], (uint8_t)(y0 + 0x10 - 0x10));

    /* No wheel and no pan on this bus, so those bytes stay put. */
    ASSERT_EQ(xram[AT_MOU + 3], 0);
    ASSERT_EQ(xram[AT_MOU + 4], 0);
}

/* The same mouse report is a pointer to tab.c, which is how the tablet
 * driver gets hooked up at all on this platform. */
UTEST(apf, a_mouse_is_also_a_pointer)
{
    reset_all();
    tab_xreg(0x4000);
    apf_mount(3, APF_TYPE_MOU);

    feed(3, APF_TYPE_MOU, 0x50000001u, 0x00000000u, 0x0000, true);
    /* Contact zero's byte 0 carries the state bits; a pointer that has
     * reported at all is present. */
    uint8_t before = xram[0x4000 + 4];
    feed(3, APF_TYPE_MOU, 0x50000200u, 0x00010001u, 0x0001, false);
    ASSERT_NE(xram[0x4000 + 4], (uint8_t)(before ^ 0xFF));
    /* The button reached it: tab.c's tip switch is the first button. */
    ASSERT_NE(xram[0x4000 + 4] & 1, 0);
}

/* Mapping a driver blanks the record it maps, and these registers are
 * levels: a stick held off centre reports the same thing forever, so
 * the pacing above would never send it again. A program that maps the
 * pad with something held must still see it rather than a blank record
 * that only fills when the hand moves. */
UTEST(apf, mapping_a_driver_republishes_what_is_held)
{
    reset_all();
    apf_mount(0, APF_TYPE_PAD_ANA);

    /* Held, off centre, and delivered. */
    const uint32_t held = 0x30000000u | (1u << 15); /* start */
    feed(0, APF_TYPE_PAD_ANA, held, 0x808000FFu, 0x0000, false);
    ASSERT_EQ(pad_rec(0)[PAD_BUTTON1] & (1u << 3), 1u << 3);
    ASSERT_EQ((int8_t)pad_rec(0)[PAD_LX], 127);

    /* The program maps the pad, which blanks every record. */
    pad_xreg(AT_PAD);
    ASSERT_EQ(pad_rec(0)[PAD_BUTTON1] & (1u << 3), 0);
    ASSERT_EQ((int8_t)pad_rec(0)[PAD_LX], 0);

    /* The hand has not moved, so the next pass builds a report byte for
     * byte identical to the one already sent. Only apf_refresh makes it
     * news again. */
    apf_refresh();
    feed(0, APF_TYPE_PAD_ANA, held, 0x808000FFu, 0x0000, false);
    ASSERT_EQ(pad_rec(0)[PAD_BUTTON1] & (1u << 3), 1u << 3);
    ASSERT_EQ((int8_t)pad_rec(0)[PAD_LX], 127);
}

/* A slot is what its type nibble says, not what its number is. */
UTEST(apf, a_slot_holds_what_its_type_says)
{
    reset_all();

    /* A keyboard on the slot Analogue puts a pad on. */
    apf_mount(0, APF_TYPE_KBD);
    feed(0, APF_TYPE_KBD, 0x40000000u, 0x04000000u, 0x0000, false);
    ASSERT_EQ(xram[AT_KBD + (0x04 >> 3)] & (1 << (0x04 & 7)), 1 << (0x04 & 7));
    /* And no pad appeared for it. */
    ASSERT_EQ(pad_rec(0)[PAD_DPAD] & 0x80, 0);

    /* Unplugged and replaced by a pad, on the same slot. */
    apf_umount(0);
    apf_mount(0, APF_TYPE_PAD_ANA);
    feed(0, APF_TYPE_PAD_ANA, 0x30000000u | (1u << 4), 0x80808080u, 0x0000,
         false);
    ASSERT_EQ(pad_rec(0)[PAD_DPAD] & 0x80, 0x80);
    ASSERT_EQ(pad_rec(0)[PAD_BUTTON0], 1u << 0);
}

UTEST_MAIN();
