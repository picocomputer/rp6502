/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _DES_H_
#define _DES_H_

#include <stdint.h>
#include <stdbool.h>

#define PAD_MAX_BUTTONS 20

// All types of gamepad reports are normalized to this structure.
// - Xinput, which is a custom protocol for the Xbox.
// - Sony, who uses HID but doesn't provide a descritor.
// - HID, aka Dinput, aka the wild west of gamepads.
// idx is the HID idx when 0..CFG_TUH_HID
// idx is from Xinput when CFG_TUH_HID..CFG_TUH_HID+PAD_MAX_PLAYERS
typedef struct
{
    bool valid;
    bool sony;
    bool hid;
    uint8_t idx;       // HID is 0..CFG_TUH_HID, Xinput is CFG_TUH_HID..CFG_TUH_HID+PAD_MAX_PLAYERS
    uint8_t report_id; // If non zero, the first report byte must match and will be skipped
    uint16_t x_offset; // Left stick X
    uint8_t x_size;
    int32_t x_logical_min;
    int32_t x_logical_max;
    uint16_t y_offset; // Left stick Y
    uint8_t y_size;
    int32_t y_logical_min;
    int32_t y_logical_max;
    uint16_t z_offset; // Right stick X (Z axis)
    uint8_t z_size;
    int32_t z_logical_min;
    int32_t z_logical_max;
    uint16_t rz_offset; // Right stick Y (Rz axis)
    uint8_t rz_size;
    int32_t rz_logical_min;
    int32_t rz_logical_max;
    uint16_t rx_offset; // Left trigger (Rx axis)
    uint8_t rx_size;
    int32_t rx_logical_min;
    int32_t rx_logical_max;
    uint16_t ry_offset; // Right trigger (Ry axis)
    uint8_t ry_size;
    int32_t ry_logical_min;
    int32_t ry_logical_max;
    uint16_t hat_offset; // D-pad/hat
    uint8_t hat_size;
    int32_t hat_logical_min;
    int32_t hat_logical_max;
    // Button bit offsets, 0xFFFF = unused
    uint16_t button_offsets[PAD_MAX_BUTTONS];
} des_gamepad_t;

/**
 * @brief Parse a HID report descriptor for a gamepad and populate a descriptor structure.
 *
 * This function parses the provided HID report descriptor data for a gamepad device and fills
 * the given des_gamepad_t structure with normalized information about axes, buttons, and hats.
 * It supports a variety of gamepad protocols, including Xinput, Sony, and generic HID (Dinput).
 *
 * For Xinput and Sony controllers, a zero-length report is accepted; in these cases, the function
 * will use other means of detection and normalization instead of parsing the descriptor data.
 *
 * @param desc         Pointer to the des_gamepad_t structure to populate with parsed data.
 * @param desc_report  Pointer to the HID report descriptor data buffer.
 * @param desc_len     Length (in bytes) of the HID report descriptor data (may be zero for Xinput/Sony).
 * @param dev_addr     Device address (Bluetooth or USB address of the gamepad).
 * @param vendor_id    USB or Bluetooth vendor ID of the device.
 * @param product_id   USB or Bluetooth product ID of the device.
 */
void des_report_descriptor(des_gamepad_t *desc,
                           uint8_t const *desc_report, uint16_t desc_len,
                           uint8_t dev_addr, uint16_t vendor_id, uint16_t product_id);

#endif // _DES_H_
