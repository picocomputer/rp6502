/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_HID_PARSE_H_
#define _CORE_HID_PARSE_H_

/* A HID report descriptor, read into the structs the drivers process
 * reports with. A machine that states its devices instead of meeting
 * them does not link this.
 */

#include "core/hid/hid.h"
#include "core/hid/keyboard.h"
#include "core/hid/mouse.h"
#include "core/hid/gamepad.h"
#include "core/hid/tablet.h"

typedef struct
{
    keyboard_connection_t keyboard;
    mouse_connection_t mouse;
    tablet_connection_t tablet;
    gamepad_connection_t gamepad;
} hid_parsed_t;

/* One walk fills all four. Each carries its own valid flag saying whether
 * the descriptor described that kind of device at all. */
void hid_parse(const uint8_t *desc, uint16_t desc_len, hid_parsed_t *out);

#endif /* _CORE_HID_PARSE_H_ */
