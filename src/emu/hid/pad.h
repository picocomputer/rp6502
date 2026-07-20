/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_HID_PAD_H_
#define _EMU_HID_PAD_H_

#include <stdbool.h>
#include <stdint.h>

/* This is a standalone REPLACE twin (not `#include "ria/hid/pad.h"`): the web
 * shell's fixed JS-ABI export `pad_report` in host/web/exports.c collides with
 * the firmware's `pad_report` HID-parser prototype, so the base header can't be
 * pulled in here. */

/* Flat button id spanning the firmware report's dpad/button0/button1 fields.
 * pad_hid_set maps each to its (byte, bit) in the player record. */
typedef enum
{
    PAD_BTN_DPAD_UP,
    PAD_BTN_DPAD_DOWN,
    PAD_BTN_DPAD_LEFT,
    PAD_BTN_DPAD_RIGHT,
    PAD_BTN_A,
    PAD_BTN_B,
    PAD_BTN_X,
    PAD_BTN_Y,
    PAD_BTN_L1,
    PAD_BTN_R1,
    PAD_BTN_L2,
    PAD_BTN_R2,
    PAD_BTN_SELECT,
    PAD_BTN_START,
    PAD_BTN_HOME,
    PAD_BTN_L3,
    PAD_BTN_R3,
} pad_button_t;

/* xreg_ria_gamepad API: point the 4-player report block (10 bytes each) at an
 * XRAM address (0xFFFF = off). Mirrors ria/hid/pad.c pad_xreg. */
bool pad_set_xram(uint16_t addr);

/* Plug/unplug a virtual gamepad (sets the connected bit programs gate on);
 * unplugging blanks the whole record. Players 0..3. */
void pad_connect(int player, bool connected);

/* Toggle one button/dpad direction for a connected player. */
void pad_hid_set(int player, pad_button_t button, bool down);

/* True once a program has pointed the report block at XRAM (xreg_ria_gamepad).
 * The web shell gates all browser Gamepad-API access on this. */
bool pad_is_mapped(void);

/* Publish a connected player's full report from host input. dpad uses bits
 * up/down/left/right = 0x01/0x02/0x04/0x08; button0/button1 use the canonical
 * layout (see pad.c). Analog sticks are -128..127, triggers 0..255; the digital
 * sticks byte and the L2/R2 trigger<->button coupling are derived to mirror the
 * firmware. The connected (and sony) feature bits are set here. */
void pad_host_report(int player, uint8_t dpad, uint8_t button0, uint8_t button1,
                     int lx, int ly, int rx, int ry, int lt, int rt, bool sony);

void pad_stop(void);

#endif /* _EMU_HID_PAD_H_ */
