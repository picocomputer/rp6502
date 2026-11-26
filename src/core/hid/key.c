/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/key.h"
#include "core/hid/clip.h"
#include "core/hid/typed.h"
#include "core/hid/usage.h"
#include "core/api/proc.h"
#include "core/sys/sst.h"
#include "core/term/vterm_mouse.h"
#include "host/host.h"
#include "machine.h"

/* The key whose press was last a chord, or 0. */
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

/* A copy or paste key acts. From flash, as the calls it makes are. */
static void HOST_IN_FLASH_FUNC key_clip_act(key_clip_t clip)
{
    if (clip == KEY_CLIP_PASTE)
        clip_paste(CLIP_CLIPBOARD);
    else if (!vterm_mouse_copy())
        typed_put(TYPED_ENTRY(TYPED_COPY, 0));
}

key_chord_t HOST_IN_RAM_FUNC key_chord(uint8_t keycode, uint8_t modifier, bool repeat)
{
    if (repeat)
        return keycode && keycode == key_chord_down ? KEY_CHORD_ACTED : KEY_CHORD_NONE;
    const key_clip_t clip = key_clip(keycode, modifier);
    key_chord_t chord = KEY_CHORD_NONE;
    if (clip == KEY_CLIP_CUT)
        chord = KEY_CHORD_CUT;
    else if (clip != KEY_CLIP_NONE)
    {
        key_clip_act(clip);
        chord = KEY_CHORD_ACTED;
    }
    else if (proc_key_chord(keycode, modifier & KEY_CTRL, modifier & KEY_ALT))
        chord = KEY_CHORD_ACTED;
    key_chord_down = chord ? keycode : 0;
    return chord;
}
