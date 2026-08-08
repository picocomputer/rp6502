/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The HID keyboard bitmap of ria/hid/kbd.c, fed by the dock's keyboard
 * instead of TinyUSB. A program maps the 256-bit bitmap into XRAM
 * through the device-0 xreg. Word 0's low bits are reserved: bit 0 says
 * no keys are pressed, bits 1-3 mirror the lock LEDs — no lock handling
 * here, so they stay clear.
 *
 * APF hands over a report, not events: up to six scan codes and the
 * modifiers, and it says plainly that "the order in which scan codes
 * appear in the registers is implementation-dependent". So the six are
 * a set and the bitmap is rebuilt from the whole of it every poll —
 * which is what a USB keyboard sends anyway. Watching for edges in a
 * set whose order is not promised would invent releases every time a
 * key changed slots.
 */

#include "kbd.h"
#include "mmio.h"

#include "ria/sys/mem.h"

#include <string.h>

static uint32_t kbd_keys[8] = {1}; /* idle: no keys down */
static uint16_t kbd_xram = 0xFFFF; /* 0xFFFF = not mapped */

static void kbd_write_xram(void)
{
    kbd_keys[0] &= ~0xFu;
    bool any = false;
    for (int k = 0; k < 8; k++)
        if (kbd_keys[k])
            any = true;
    if (!any)
        kbd_keys[0] |= 1;
    if (kbd_xram != 0xFFFF)
        memcpy(&xram[kbd_xram], kbd_keys, sizeof(kbd_keys));
}

bool kbd_set_xram(uint16_t addr)
{
    if (addr != 0xFFFF && addr > 0x10000 - sizeof(kbd_keys))
        return false;
    kbd_xram = addr;
    kbd_write_xram();
    return true;
}

static void kbd_press(uint8_t code)
{
    /* Keycodes 0-3 are reserved; their bits in word 0 are the flags. */
    if (code >= 4)
        kbd_keys[code >> 5] |= 1u << (code & 31);
}

void kbd_task(void)
{
    uint32_t mod = MMIO_KBD_KEY;
    uint32_t codes = MMIO_KBD_JOY;
    uint32_t more = MMIO_KBD_TRIG;

    memset(kbd_keys, 0, sizeof(kbd_keys));
    /* Type 4 is the dock's keyboard; anything else is not one, and its
     * words mean something else entirely. */
    if ((mod >> 28) == 4) {
        kbd_press((uint8_t)(codes >> 24));
        kbd_press((uint8_t)(codes >> 16));
        kbd_press((uint8_t)(codes >> 8));
        kbd_press((uint8_t)codes);
        kbd_press((uint8_t)(more >> 8));
        kbd_press((uint8_t)more);
        /* The eight standard modifiers are keycodes 0xE0 to 0xE7, in
         * the order the HID report byte packs them. */
        for (int i = 0; i < 8; i++)
            if (mod & (1u << i))
                kbd_press((uint8_t)(0xE0 + i));
    }
    kbd_write_xram();
}

void kbd_stop(void)
{
    memset(kbd_keys, 0, sizeof(kbd_keys));
    kbd_keys[0] = 1;
    kbd_xram = 0xFFFF;
}
