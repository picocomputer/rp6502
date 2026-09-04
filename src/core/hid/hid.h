/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_HID_HID_H_
#define _CORE_HID_HID_H_

/* Common code shared among all HID and HID-like drivers.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Lock LEDs, fanned out to every attached keyboard on every transport this
 * platform has. */
void hid_set_leds(uint8_t leds);

/* True while the platform is still enumerating boot devices, so the keyboard
 * holds off deciding a layout. False where enumeration is not a thing. */
bool hid_boot_enumerating(void);

/* An XREG write just pointed a device at a new XRAM report block, which blanks
 * the record. Where this machine's transport holds the current state rather
 * than resending it on its own, that state has to go out again -- a control
 * standing still is news to a record that was just emptied. Nothing to do
 * where reports arrive continuously. */
void hid_remapped(void);

uint32_t hid_extract_bits(const uint8_t *report, uint16_t report_len, uint16_t bit_offset, uint8_t bit_size);
int32_t hid_extract_signed(const uint8_t *report, uint16_t report_len, uint16_t bit_offset, uint8_t bit_size);
uint8_t hid_scale_analog(uint32_t raw_value, uint8_t bit_size, int32_t logical_min, int32_t logical_max);
int8_t hid_scale_analog_signed(uint32_t raw_value, uint8_t bit_size, int32_t logical_min, int32_t logical_max);

/* What a device says it is, from the Application Collection the field sits
 * in: (usage page << 16) | usage. Zero when the descriptor declared none,
 * which is when a driver is left guessing from the fields that turned up. */
#define HID_APP_NONE 0
#define HID_APP_POINTER 0x00010001
#define HID_APP_MOUSE 0x00010002
#define HID_APP_JOYSTICK 0x00010004
#define HID_APP_GAMEPAD 0x00010005
#define HID_APP_KEYBOARD 0x00010006
#define HID_APP_DIGITIZER 0x000D0001
#define HID_APP_PEN 0x000D0002
#define HID_APP_TOUCH 0x000D0004

// Where a field sits in a report; an absent one says so with this.
#define HID_ABSENT 0xFFFF

/* The drivers' own structs are what a device is offered as. Their headers
 * include this one, so they are named here rather than included. */
typedef struct keyboard_connection keyboard_connection_t;
typedef struct mouse_connection mouse_connection_t;
typedef struct tablet_connection tablet_connection_t;
typedef struct gamepad_connection gamepad_connection_t;

#define HID_CLAIM_KEYBOARD (1 << 0)
#define HID_CLAIM_MOUSE (1 << 1)
#define HID_CLAIM_TABLET (1 << 2)
#define HID_CLAIM_PAD (1 << 3)

/* Offer a device to the drivers it might be, and keep which ones took it.
 * A NULL says the device is not one of those -- never a zeroed struct,
 * which reads as a real device with everything at bit zero. Returns the
 * slot it was given, or -1 if none took it or there is no room. The
 * interface keeps that slot beside whatever else it knows about the
 * device; nothing here can name a device on its behalf.
 *
 * Deliberately not exclusive: a mouse is a pointer to both mouse and tablet. */
int hid_mount(const keyboard_connection_t *keyboard, const mouse_connection_t *mouse,
              const tablet_connection_t *tablet, const gamepad_connection_t *gamepad,
              uint16_t vendor_id, uint16_t product_id, uint8_t button_type);

// Hand a report to the drivers that claimed the slot, and no others.
void hid_report(int slot, const uint8_t *data, uint16_t len);

void hid_umount(int slot);

uint8_t hid_slot_claims(int slot);

#endif /* _CORE_HID_HID_H_ */
