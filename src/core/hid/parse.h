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
#include "core/hid/mou.h"
#include "core/hid/pad.h"
#include "core/hid/tab.h"

typedef struct
{
    keyboard_connection_t keyboard;
    mou_connection_t mou;
    tab_connection_t tab;
    pad_connection_t pad;
} hid_parsed_t;

/* One walk fills all four. Each carries its own valid flag saying whether
 * the descriptor described that kind of device at all. */
void hid_parse(const uint8_t *desc, uint16_t desc_len, hid_parsed_t *out);

#endif /* _CORE_HID_PARSE_H_ */
