/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * A gamepad button by name, for the callers that take one as text: a
 * script, a config file, a host's key bindings.
 */

#include "core/emu/hid/pad.h"
#include <stddef.h>
#include <strings.h>

static const struct
{
    const char *name;
    pad_button_t button;
} pad_names[] = {
    {"up", PAD_BTN_DPAD_UP},
    {"down", PAD_BTN_DPAD_DOWN},
    {"left", PAD_BTN_DPAD_LEFT},
    {"right", PAD_BTN_DPAD_RIGHT},
    {"a", PAD_BTN_A},
    {"b", PAD_BTN_B},
    {"c", PAD_BTN_C},
    {"x", PAD_BTN_X},
    {"y", PAD_BTN_Y},
    {"z", PAD_BTN_Z},
    {"l1", PAD_BTN_L1},
    {"r1", PAD_BTN_R1},
    {"l2", PAD_BTN_L2},
    {"r2", PAD_BTN_R2},
    {"select", PAD_BTN_SELECT},
    {"start", PAD_BTN_START},
    {"home", PAD_BTN_HOME},
    {"l3", PAD_BTN_L3},
    {"r3", PAD_BTN_R3},
};

bool pad_button_from_name(const char *name, pad_button_t *button)
{
    if (!name)
        return false;
    for (size_t i = 0; i < sizeof pad_names / sizeof pad_names[0]; i++)
        if (!strcasecmp(name, pad_names[i].name))
        {
            *button = pad_names[i].button;
            return true;
        }
    return false;
}
