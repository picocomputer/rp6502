/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _DES_H_
#define _DES_H_

#include <stdint.h>
#include <stdbool.h>

#define PAD_MAX_BUTTONS 24

// HID Report Descriptor parsing state
typedef struct
{
    bool valid;
    uint8_t report_id;
    uint16_t x_offset; // Left stick X
    uint8_t x_size;
    uint16_t y_offset; // Left stick Y
    uint8_t y_size;
    uint16_t z_offset; // Right stick X (Z axis)
    uint8_t z_size;
    uint16_t rz_offset; // Right stick Y (Rz axis)
    uint8_t rz_size;
    uint16_t rx_offset; // Left trigger (Rx axis)
    uint8_t rx_size;
    uint16_t ry_offset; // Right trigger (Ry axis)
    uint8_t ry_size;
    uint16_t hat_offset; // D-pad/hat
    uint8_t hat_size;
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
                                 uint16_t vendor_id, uint16_t product_id);

#endif // _DES_H_
