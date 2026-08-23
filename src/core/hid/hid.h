/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_HID_HID_H_
#define _CORE_HID_HID_H_

/* Common code shared among all HID and HID-like drivers.
 */

#include "host.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Lock LEDs, fanned out to every attached keyboard on every transport this
 * platform has. */
void hid_set_leds(uint8_t leds);

/* True while the platform is still enumerating boot devices, so the keyboard
 * holds off deciding a layout. False where enumeration is not a thing. */
bool hid_boot_enumerating(void);

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

// Per-field info yielded by hid_descriptor_parse.
typedef struct
{
    uint32_t app_usage;
    uint16_t usage_page;
    uint16_t usage;
    /* An Array field's slots each hold one usage out of this range, so the
     * range is the field's identity where usage is a Variable field's. */
    uint16_t usage_min;
    uint16_t usage_max;
    uint16_t report_id; // 0xFFFF if no report ID
    uint16_t bit_pos;   // Bit offset within report (excludes report ID byte)
    uint8_t size;       // Field size in bits
    int32_t logical_min;
    int32_t logical_max;
    uint8_t input_flags; // Raw Input item flags byte (bit 1 = variable, bit 2 = relative)
} hid_field_t;

#define HID_FIELD_IS_ARRAY(f) (!((f)->input_flags & 0x02))

// Callback for each Input field. Return false to stop parsing.
typedef bool (*hid_field_cb_t)(const hid_field_t *field, void *context);

// Parse a raw HID report descriptor byte stream.
void hid_descriptor_parse(const uint8_t *desc, uint16_t desc_len, hid_field_cb_t cb, void *context);

// Where a field sits in a report. bit_pos 0xFFFF means the device has none.
#define HID_ABSENT 0xFFFF
typedef struct
{
    uint16_t bit_pos;
    uint8_t size;
    bool relative;
    int32_t logical_min;
    int32_t logical_max;
} hid_locus_t;

/* The axes any of the drivers ask for. Simulation-page accelerator and brake
 * fold onto RX and RY, which is where a gamepad's triggers report. */
typedef enum
{
    HID_AXIS_X,
    HID_AXIS_Y,
    HID_AXIS_Z,
    HID_AXIS_RX,
    HID_AXIS_RY,
    HID_AXIS_RZ,
    HID_AXIS_HAT,
    HID_AXIS_WHEEL,
    HID_AXIS_PAN,
    HID_AXIS_COUNT
} hid_axis_t;

#define HID_MAP_BUTTONS 20

/* A run of consecutive keyboard usages, one bit each -- how both the boot
 * keyboard's modifier byte and an NKRO bitmap are actually declared. */
#define HID_MAP_KEY_RUNS 4
typedef struct
{
    uint16_t bit_pos;   // bit of usage_min
    uint16_t usage_min;
    uint16_t count;
} hid_key_run_t;

/* One report, located. Everything the four drivers pull out of a descriptor,
 * which is also everything a host without one has to state. */
typedef struct
{
    uint32_t app_usage; // what the device says it is, HID_APP_NONE if it didn't
    uint8_t report_id;  // 0 = the device uses none
    hid_locus_t axis[HID_AXIS_COUNT];
    uint16_t button_bit[HID_MAP_BUTTONS]; // HID_ABSENT when the device lacks it
    uint16_t tip_bit;                     // Digitizer Tip Switch
    uint16_t inrange_bit;                 // Digitizer In Range
    uint16_t key_array_bit;               // contiguous 8-bit keycodes
    uint8_t key_array_count;
    hid_key_run_t key_run[HID_MAP_KEY_RUNS];
} hid_report_map_t;

// Every locus absent, every button absent, no app usage.
void hid_map_clear(hid_report_map_t *map);

/* One walk of the descriptor, filling the map. False when the descriptor
 * describes nothing any driver could use. */
bool hid_map_from_descriptor(hid_report_map_t *map, const uint8_t *desc, uint16_t desc_len);

/* Which interface a device arrived on, and the interface's own name for
 * it. The two together are what a device is looked up by; the slot a
 * mount hands back is what the drivers know it as. */
typedef enum
{
    HID_IFACE_USB,
    HID_IFACE_XIN,
    HID_IFACE_BLE,
    HID_IFACE_APF,
    HID_IFACE_HOST, // a desktop OS, which decodes its own devices
} hid_iface_t;

/* Devices that at least one driver wanted. A machine with more physical
 * ports than this simply runs out, which is what the drivers do too. A
 * host that knows it has fewer says so in its host.h. */
#ifndef HID_MAX_SLOTS
#define HID_MAX_SLOTS 16
#endif

#define HID_CLAIM_KBD (1 << 0)
#define HID_CLAIM_MOU (1 << 1)
#define HID_CLAIM_TAB (1 << 2)
#define HID_CLAIM_PAD (1 << 3)

/* Offer a device to every driver and keep which ones took it. Returns
 * its slot, or -1 if none did or there is no room. Deliberately not
 * exclusive: a mouse is a pointer to both mou and tab. */
int hid_mount(hid_iface_t iface, uint32_t key, const hid_report_map_t *map,
              uint16_t vendor_id, uint16_t product_id, uint8_t button_type);

// Hand a report to the drivers that claimed the slot, and no others.
void hid_report(int slot, const uint8_t *data, uint16_t len);

void hid_umount(int slot);

// The slot an interface's device is mounted in, or -1.
int hid_slot(hid_iface_t iface, uint32_t key);

// What the interface called it, for a transport that has to answer back.
uint32_t hid_slot_key(int slot);

uint8_t hid_slot_claims(int slot);

#endif /* _CORE_HID_HID_H_ */
