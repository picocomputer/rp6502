/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/key.h"
#include "core/hid/clip.h"
#include "core/hid/usage.h"
#include "core/api/proc.h"
#include "core/com/rln.h"
#include "core/sys/sst.h"
#include "host/host.h"
#include "machine.h"

/* The key whose press last acted as a chord, or 0. */
static uint8_t key_chord_down;

#define KEY_SST(X) \
    X(SKIP, key_chord_down, "host keyboard state, like keymap's repeat")
SST_TABLE(key, KEY_SST)

#define KEY_CTRL (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL)
#define KEY_SHIFT (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)
#define KEY_ALT (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT)
#define KEY_GUI (KEYBOARD_MODIFIER_LEFTGUI | KEYBOARD_MODIFIER_RIGHTGUI)

/* Each chord's modifiers are exactly these, either side of each. */
key_clip_t HOST_IN_RAM_FUNC key_clip(uint8_t keycode, uint8_t modifier)
{
    const unsigned held = (modifier & KEY_CTRL ? KEY_CTRL : 0) |
                          (modifier & KEY_SHIFT ? KEY_SHIFT : 0) |
                          (modifier & KEY_ALT ? KEY_ALT : 0) |
                          (modifier & KEY_GUI ? KEY_GUI : 0);
    const bool clip = held == (KEY_CTRL | KEY_SHIFT) || (held == KEY_GUI && host_mac_keys());
    switch (keycode)
    {
    case HID_KEY_C:
        return clip ? KEY_CLIP_COPY : KEY_CLIP_NONE;
    case HID_KEY_V:
        return clip ? KEY_CLIP_PASTE : KEY_CLIP_NONE;
    case HID_KEY_INSERT:
        return held == KEY_CTRL    ? KEY_CLIP_COPY
               : held == KEY_SHIFT ? KEY_CLIP_PASTE
                                   : KEY_CLIP_NONE;
    case HID_KEY_X:
        return clip ? KEY_CLIP_CUT : KEY_CLIP_NONE;
    case HID_KEY_DELETE:
        return held == KEY_SHIFT ? KEY_CLIP_CUT : KEY_CLIP_NONE;
    default:
        return KEY_CLIP_NONE;
    }
}

/* From flash, as the clip and line editor calls it makes are. */
bool HOST_IN_FLASH_FUNC key_clip_act(key_clip_t clip)
{
    switch (clip)
    {
    case KEY_CLIP_COPY:
        if (!rln_copy())
            clip_copy_primary();
        return true;
    case KEY_CLIP_CUT:
        return rln_cut();
    case KEY_CLIP_PASTE:
        clip_paste(CLIP_CLIPBOARD);
        return true;
    default:
        return false;
    }
}

bool key_clip_acts(uint8_t keycode, uint8_t modifier, bool repeat)
{
    switch (key_clip(keycode, modifier))
    {
    case KEY_CLIP_COPY:
    case KEY_CLIP_PASTE:
        return true;
    case KEY_CLIP_CUT:
        return repeat ? keycode == key_chord_down : rln_reading();
    default:
        return false;
    }
}

bool HOST_IN_RAM_FUNC key_chord(uint8_t keycode, uint8_t modifier, bool repeat)
{
    if (repeat)
        return keycode && keycode == key_chord_down;
    const key_clip_t clip = key_clip(keycode, modifier);
    const bool acted = (clip != KEY_CLIP_NONE && key_clip_act(clip)) ||
                       proc_key_chord(keycode, modifier & KEY_CTRL, modifier & KEY_ALT);
    key_chord_down = acted ? keycode : 0;
    return acted;
}
