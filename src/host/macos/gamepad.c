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

#include "host/sokol/gamepad_input.h"

#import <GameController/GameController.h>

static bool gamepad_macos_open;

/* Apple hands out GCController objects, not indices. The pointer is the id
 * for as long as the controller is connected, which is exactly as long as a
 * player should keep their number. */
static uint64_t gamepad_macos_id(GCController *controller)
{
    return (uint64_t)(uintptr_t)controller;
}

static uint8_t gamepad_macos_type(GCController *controller)
{
    /* Only what Apple names outright. productCategory is a display string, so
     * this reads the ones that are documented and claims nothing otherwise. */
    NSString *category = controller.productCategory;
    if (!category)
        return GAMEPAD_TYPE_UNKNOWN;
    if ([category containsString:@"DualSense"] ||
        [category containsString:@"DualShock"])
        return GAMEPAD_TYPE_PLAYSTATION;
    if ([category containsString:@"Xbox"])
        return GAMEPAD_TYPE_WESTERN;
    if ([category containsString:@"Switch"] ||
        [category containsString:@"Joy-Con"])
        return GAMEPAD_TYPE_EASTERN;
    return GAMEPAD_TYPE_UNKNOWN;
}

static int8_t gamepad_macos_axis(float value)
{
    /* The web shell's rounding, so the same stick reads the same in both. */
    float scaled = value * 127.0f;
    if (scaled > 127.0f)
        scaled = 127.0f;
    if (scaled < -128.0f)
        scaled = -128.0f;
    return (int8_t)lroundf(scaled);
}

static uint8_t gamepad_macos_trigger(float value)
{
    float scaled = value * 255.0f;
    if (scaled > 255.0f)
        scaled = 255.0f;
    if (scaled < 0.0f)
        scaled = 0.0f;
    return (uint8_t)lroundf(scaled);
}

bool host_gamepad_open(void)
{
    /* No startWirelessControllerDiscovery: it asks the user for Bluetooth
     * permission and wants an Info.plist string to explain why. Controllers
     * paired with the system are already in the list. */
    gamepad_macos_open = true;
    return true;
}

void host_gamepad_close(void)
{
    gamepad_macos_open = false;
}

int host_gamepad_poll(gamepad_host_t *gamepads, int max)
{
    if (!gamepad_macos_open)
        return 0;

    int count = 0;
    for (GCController *controller in [GCController controllers])
    {
        if (count >= max)
            break;
        GCExtendedGamepad *gc = controller.extendedGamepad;
        if (!gc)
            continue; /* a remote or a micro gamepad is not one of these */

        gamepad_host_t *gamepad = &gamepads[count++];
        memset(gamepad, 0, sizeof(*gamepad));
        gamepad->id = gamepad_macos_id(controller);
        gamepad->type = gamepad_macos_type(controller);
        gamepad->sticks = gc.leftThumbstick != nil && gc.rightThumbstick != nil;

        const struct
        {
            GCControllerButtonInput *input;
            gamepad_button_t button;
        } buttons[] = {
            {gc.buttonA, GAMEPAD_BTN_A},
            {gc.buttonB, GAMEPAD_BTN_B},
            {gc.buttonX, GAMEPAD_BTN_X},
            {gc.buttonY, GAMEPAD_BTN_Y},
            {gc.leftShoulder, GAMEPAD_BTN_L1},
            {gc.rightShoulder, GAMEPAD_BTN_R1},
            {gc.leftTrigger, GAMEPAD_BTN_L2},
            {gc.rightTrigger, GAMEPAD_BTN_R2},
            {gc.buttonOptions, GAMEPAD_BTN_SELECT},
            {gc.buttonMenu, GAMEPAD_BTN_START},
            {gc.buttonHome, GAMEPAD_BTN_HOME},
            {gc.leftThumbstickButton, GAMEPAD_BTN_L3},
            {gc.rightThumbstickButton, GAMEPAD_BTN_R3},
            {gc.dpad.up, GAMEPAD_BTN_DPAD_UP},
            {gc.dpad.down, GAMEPAD_BTN_DPAD_DOWN},
            {gc.dpad.left, GAMEPAD_BTN_DPAD_LEFT},
            {gc.dpad.right, GAMEPAD_BTN_DPAD_RIGHT},
        };
        for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; i++)
            if (buttons[i].input)
                gamepad_button_apply(buttons[i].button, buttons[i].input.isPressed,
                                     &gamepad->dpad, &gamepad->button0, &gamepad->button1);

        gamepad->lx = gamepad_macos_axis(gc.leftThumbstick.xAxis.value);
        gamepad->rx = gamepad_macos_axis(gc.rightThumbstick.xAxis.value);
        /* Apple's sticks are up-positive and the report's are down-positive. */
        gamepad->ly = gamepad_macos_axis(-gc.leftThumbstick.yAxis.value);
        gamepad->ry = gamepad_macos_axis(-gc.rightThumbstick.yAxis.value);
        gamepad->lt = gamepad_macos_trigger(gc.leftTrigger.value);
        gamepad->rt = gamepad_macos_trigger(gc.rightTrigger.value);
    }
    return count;
}
