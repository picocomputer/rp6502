/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * core/hid/kbt.h, answered by a machine that has an OS to ask.
 *
 * core/hid/kbd.c offers every key it sees to the terminal half so a
 * firmware can decide what it types. Here the host decided that before
 * the keystroke arrived -- see kbd.c, which takes the text -- so these
 * are the seam and nothing more.
 */

#include "core/hid/kbd.h"
#include "core/hid/kbt.h"

void kbt_init(void)
{
}

void kbd_spell_key(uint8_t modifier, uint8_t keycode)
{
    (void)modifier;
    (void)keycode;
}

void kbd_spell_modifiers(uint8_t modifier)
{
    (void)modifier;
}
