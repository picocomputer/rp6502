/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * A gamepad button by name, for the callers that take one as text: a
 * script, a config file, a host's key bindings.
 */

#include "host/sokol/gamepad.h"
#include <stddef.h>
#include <strings.h>

static const struct
{
    const char *name;
    gamepad_button_t button;
} gamepad_names[] = {
    {"up", GAMEPAD_BTN_DPAD_UP},
    {"down", GAMEPAD_BTN_DPAD_DOWN},
    {"left", GAMEPAD_BTN_DPAD_LEFT},
    {"right", GAMEPAD_BTN_DPAD_RIGHT},
    {"a", GAMEPAD_BTN_A},
    {"b", GAMEPAD_BTN_B},
    {"c", GAMEPAD_BTN_C},
    {"x", GAMEPAD_BTN_X},
    {"y", GAMEPAD_BTN_Y},
    {"z", GAMEPAD_BTN_Z},
    {"l1", GAMEPAD_BTN_L1},
    {"r1", GAMEPAD_BTN_R1},
    {"l2", GAMEPAD_BTN_L2},
    {"r2", GAMEPAD_BTN_R2},
    {"select", GAMEPAD_BTN_SELECT},
    {"start", GAMEPAD_BTN_START},
    {"home", GAMEPAD_BTN_HOME},
    {"l3", GAMEPAD_BTN_L3},
    {"r3", GAMEPAD_BTN_R3},
};

bool gamepad_button_from_name(const char *name, gamepad_button_t *button)
{
    if (!name)
        return false;
    for (size_t i = 0; i < sizeof gamepad_names / sizeof gamepad_names[0]; i++)
        if (!strcasecmp(name, gamepad_names[i].name))
        {
            *button = gamepad_names[i].button;
            return true;
        }
    return false;
}
