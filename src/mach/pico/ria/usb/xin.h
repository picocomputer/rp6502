/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_USB_XIN_H_
#define _RIA_USB_XIN_H_

/* USB XInput driver for XBox gamepads.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "host/usbh.h"
#include "host/usbh_pvt.h"

// For monitor status command.
int xin_status_count(void);

// TinyUSB host class-driver callbacks.
bool xin_class_driver_init(void);
uint16_t xin_class_driver_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_interface_t const *desc_itf, uint16_t max_len);
bool xin_class_driver_set_config(uint8_t dev_addr, uint8_t itf_num);
bool xin_class_driver_xfer_cb(uint8_t dev_addr, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes);
void xin_class_driver_close(uint8_t dev_addr);

#endif /* _RIA_USB_XIN_H_ */
