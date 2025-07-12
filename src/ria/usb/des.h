/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _DES_H_
#define _DES_H_

#include <stdint.h>
#include <stdbool.h>

// HID Report Descriptor parsing state
typedef struct
{
    bool valid;
    uint8_t report_id;
    uint8_t x_offset, x_size;             // Left stick X
    uint8_t y_offset, y_size;             // Left stick Y
    uint8_t z_offset, z_size;             // Right stick X (Z axis)
    uint8_t rz_offset, rz_size;           // Right stick Y (Rz axis)
    uint8_t rx_offset, rx_size;           // Left trigger (Rx axis)
    uint8_t ry_offset, ry_size;           // Right trigger (Ry axis)
    uint8_t hat_offset, hat_size;         // D-pad/hat
    uint8_t buttons_offset, buttons_size; // Button bitmask
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
bool des_parse_report_descriptor(pad_descriptor_t *desc,
                                 uint8_t dev_addr, uint8_t const *desc_report, uint16_t desc_len,
                                 uint16_t vendor_id, uint16_t product_id);

#endif // _DES_H_
