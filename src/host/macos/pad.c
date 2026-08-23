/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * macOS gamepads, through GameController.framework. Apple's drivers know
 * DualSense, DualShock, Xbox and Switch Pro natively and hand back an already
 * normalized profile, so there is no mapping database here, and — unlike
 * IOKit — no Input Monitoring consent dialog for a user to be surprised by.
 * If one ever appears, something below has reached IOKit and that is the bug.
 *
 * Compiled as Objective-C. Controllers arrive on the main run loop, which is
 * the one sokol already spins, so this is read from the frame callback along
 * with everything else rather than from a thread.
 */

#include "core/emu/app/pad_input.h"

#import <GameController/GameController.h>

static bool pad_macos_open;

/* Apple hands out GCController objects, not indices. The pointer is the id
 * for as long as the controller is connected, which is exactly as long as a
 * player should keep their number. */
static uint64_t pad_macos_id(GCController *controller)
{
    return (uint64_t)(uintptr_t)controller;
}

static uint8_t pad_macos_type(GCController *controller)
{
    /* Only what Apple names outright. productCategory is a display string, so
     * this reads the ones that are documented and claims nothing otherwise. */
    NSString *category = controller.productCategory;
    if (!category)
        return PAD_TYPE_UNKNOWN;
    if ([category containsString:@"DualSense"] ||
        [category containsString:@"DualShock"])
        return PAD_TYPE_PLAYSTATION;
    if ([category containsString:@"Xbox"])
        return PAD_TYPE_WESTERN;
    if ([category containsString:@"Switch"] ||
        [category containsString:@"Joy-Con"])
        return PAD_TYPE_EASTERN;
    return PAD_TYPE_UNKNOWN;
}

static int8_t pad_macos_axis(float value)
{
    /* The web shell's rounding, so the same stick reads the same in both. */
    float scaled = value * 127.0f;
    if (scaled > 127.0f)
        scaled = 127.0f;
    if (scaled < -128.0f)
        scaled = -128.0f;
    return (int8_t)lroundf(scaled);
}

static uint8_t pad_macos_trigger(float value)
{
    float scaled = value * 255.0f;
    if (scaled > 255.0f)
        scaled = 255.0f;
    if (scaled < 0.0f)
        scaled = 0.0f;
    return (uint8_t)lroundf(scaled);
}

bool host_pad_open(void)
{
    /* No startWirelessControllerDiscovery: it asks the user for Bluetooth
     * permission and wants an Info.plist string to explain why. Controllers
     * paired with the system are already in the list. */
    pad_macos_open = true;
    return true;
}

void host_pad_close(void)
{
    pad_macos_open = false;
}

int host_pad_poll(pad_host_t *pads, int max)
{
    if (!pad_macos_open)
        return 0;

    int count = 0;
    for (GCController *controller in [GCController controllers])
    {
        if (count >= max)
            break;
        GCExtendedGamepad *gamepad = controller.extendedGamepad;
        if (!gamepad)
            continue; /* a remote or a micro gamepad is not one of these */

        pad_host_t *pad = &pads[count++];
        memset(pad, 0, sizeof(*pad));
        pad->id = pad_macos_id(controller);
        pad->type = pad_macos_type(controller);
        pad->sticks = gamepad.leftThumbstick != nil && gamepad.rightThumbstick != nil;

        const struct
        {
            GCControllerButtonInput *input;
            pad_button_t button;
        } buttons[] = {
            {gamepad.buttonA, PAD_BTN_A},
            {gamepad.buttonB, PAD_BTN_B},
            {gamepad.buttonX, PAD_BTN_X},
            {gamepad.buttonY, PAD_BTN_Y},
            {gamepad.leftShoulder, PAD_BTN_L1},
            {gamepad.rightShoulder, PAD_BTN_R1},
            {gamepad.leftTrigger, PAD_BTN_L2},
            {gamepad.rightTrigger, PAD_BTN_R2},
            {gamepad.buttonOptions, PAD_BTN_SELECT},
            {gamepad.buttonMenu, PAD_BTN_START},
            {gamepad.buttonHome, PAD_BTN_HOME},
            {gamepad.leftThumbstickButton, PAD_BTN_L3},
            {gamepad.rightThumbstickButton, PAD_BTN_R3},
            {gamepad.dpad.up, PAD_BTN_DPAD_UP},
            {gamepad.dpad.down, PAD_BTN_DPAD_DOWN},
            {gamepad.dpad.left, PAD_BTN_DPAD_LEFT},
            {gamepad.dpad.right, PAD_BTN_DPAD_RIGHT},
        };
        for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; i++)
            if (buttons[i].input)
                pad_button_apply(buttons[i].button, buttons[i].input.isPressed,
                                 &pad->dpad, &pad->button0, &pad->button1);

        pad->lx = pad_macos_axis(gamepad.leftThumbstick.xAxis.value);
        pad->rx = pad_macos_axis(gamepad.rightThumbstick.xAxis.value);
        /* Apple's sticks are up-positive and the report's are down-positive. */
        pad->ly = pad_macos_axis(-gamepad.leftThumbstick.yAxis.value);
        pad->ry = pad_macos_axis(-gamepad.rightThumbstick.yAxis.value);
        pad->lt = pad_macos_trigger(gamepad.leftTrigger.value);
        pad->rt = pad_macos_trigger(gamepad.rightTrigger.value);
    }
    return count;
}
