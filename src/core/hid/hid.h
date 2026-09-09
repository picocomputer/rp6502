/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_HID_HID_H_
#define _CORE_HID_HID_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

void hid_set_leds(uint8_t leds);

/* True while the platform is still enumerating the devices attached at boot.
 * The keyboard driver applies its NumLock quirk only to a keyboard that
 * arrives during that window. False where nothing enumerates. */
bool hid_boot_enumerating(void);

/* An XREG write has just pointed a device at a new XRAM report block, and the
 * gamepad and tablet blocks are blanked by that write. A transport that
 * forwards a report only when it differs from the one before has to forget
 * what it last sent, because a control held still reports the same value
 * forever and the program would read the blank until it moves again. There is
 * nothing to do where every report is forwarded. */
void hid_remapped(void);

uint32_t hid_extract_bits(const uint8_t *report, uint16_t report_len, uint16_t bit_offset, uint8_t bit_size);
int32_t hid_extract_signed(const uint8_t *report, uint16_t report_len, uint16_t bit_offset, uint8_t bit_size);
uint8_t hid_scale_analog(uint32_t raw_value, uint8_t bit_size, int32_t logical_min, int32_t logical_max);
int8_t hid_scale_analog_signed(uint32_t raw_value, uint8_t bit_size, int32_t logical_min, int32_t logical_max);

/* What a device says it is, taken from the Application Collection the field
 * sits in: (usage page << 16) | usage. Zero when the descriptor declared none,
 * which leaves a driver guessing from the fields that turned up. */
#define HID_APP_NONE 0
#define HID_APP_POINTER 0x00010001
#define HID_APP_MOUSE 0x00010002
#define HID_APP_JOYSTICK 0x00010004
#define HID_APP_GAMEPAD 0x00010005
#define HID_APP_KEYBOARD 0x00010006
#define HID_APP_DIGITIZER 0x000D0001
#define HID_APP_PEN 0x000D0002
#define HID_APP_TOUCH 0x000D0004

#define HID_ABSENT 0xFFFF

typedef struct keyboard_connection keyboard_connection_t;
typedef struct mouse_connection mouse_connection_t;
typedef struct tablet_connection tablet_connection_t;
typedef struct gamepad_connection gamepad_connection_t;

#define HID_CLAIM_KEYBOARD (1 << 0)
#define HID_CLAIM_MOUSE (1 << 1)
#define HID_CLAIM_TABLET (1 << 2)
#define HID_CLAIM_PAD (1 << 3)

/* Returns the slot the device was given, or -1. A driver the device is not is
 * passed NULL. The claims are not exclusive: a mouse is claimed by the mouse
 * driver and the tablet driver both. */
int hid_mount(const keyboard_connection_t *keyboard, const mouse_connection_t *mouse,
              const tablet_connection_t *tablet, const gamepad_connection_t *gamepad,
              uint16_t vendor_id, uint16_t product_id, uint8_t button_type);

void hid_report(int slot, const uint8_t *data, uint16_t len);

void hid_umount(int slot);

uint8_t hid_slot_claims(int slot);

#endif /* _CORE_HID_HID_H_ */
