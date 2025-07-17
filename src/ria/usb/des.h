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

// HID Report Descriptor parsing state
typedef struct
{
    bool valid;
    bool sony;
    uint8_t idx; // HID is 0..CFG_TUH_HID, Xinput is CFG_TUH_HID..CFG_TUH_HID+PAD_PLAYER_LEN
    uint8_t report_id;
    uint16_t x_offset; // Left stick X
    uint8_t x_size;
    int32_t x_logical_min; // Left stick X logical minimum
    int32_t x_logical_max; // Left stick X logical maximum
    uint16_t y_offset; // Left stick Y
    uint8_t y_size;
    int32_t y_logical_min; // Left stick Y logical minimum
    int32_t y_logical_max; // Left stick Y logical maximum
    uint16_t z_offset; // Right stick X (Z axis)
    uint8_t z_size;
    int32_t z_logical_min; // Right stick X logical minimum
    int32_t z_logical_max; // Right stick X logical maximum
    uint16_t rz_offset; // Right stick Y (Rz axis)
    uint8_t rz_size;
    int32_t rz_logical_min; // Right stick Y logical minimum
    int32_t rz_logical_max; // Right stick Y logical maximum
    uint16_t rx_offset; // Left trigger (Rx axis)
    uint8_t rx_size;
    int32_t rx_logical_min; // Left trigger logical minimum
    int32_t rx_logical_max; // Left trigger logical maximum
    uint16_t ry_offset; // Right trigger (Ry axis)
    uint8_t ry_size;
    int32_t ry_logical_min; // Right trigger logical minimum
    int32_t ry_logical_max; // Right trigger logical maximum
    uint16_t hat_offset; // D-pad/hat
    uint8_t hat_size;
    int32_t hat_logical_min; // D-pad/hat logical minimum
    int32_t hat_logical_max; // D-pad/hat logical maximum
    // Button bit offsets, 0xFFFF = unused
    uint16_t button_offsets[PAD_MAX_BUTTONS];
} pad_descriptor_t;

/**
 * @brief Parse HID report descriptor for gamepad using BTstack
 * @param descriptors Array of descriptors to store parsed data
 * @param max_descriptors Maximum number of descriptors in array
 * @param dev_addr Device address
 * @param desc_report HID report descriptor data
 * @param desc_len Length of descriptor data
 * @return true if parsing was successful, false otherwise
 */
void des_report_descriptor(pad_descriptor_t *desc,
                           uint8_t const *desc_report, uint16_t desc_len,
                           uint8_t dev_addr, uint16_t vendor_id, uint16_t product_id);

#endif // _DES_H_
