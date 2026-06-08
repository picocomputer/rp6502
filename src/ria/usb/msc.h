/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_USB_MSC_H_
#define _RIA_USB_MSC_H_

/* USB Mass Storage Controller
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "api/api.h"
#include "api/std.h"
#include "host/usbh.h"
#include "host/usbh_pvt.h"

/* Status
 */

int msc_status_count(void);
int msc_status_response(char *buf, size_t buf_size, int state);

/* TinyUSB host class-driver callbacks.
 */

bool msc_class_driver_init(void);
uint16_t msc_class_driver_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_interface_t const *desc_itf, uint16_t max_len);
bool msc_class_driver_set_config(uint8_t dev_addr, uint8_t itf_num);
bool msc_class_driver_xfer_cb(uint8_t dev_addr, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes);
void msc_class_driver_close(uint8_t dev_addr);

/* STDIO
 */

bool msc_std_handles(const char *path);
int msc_std_open(const char *path, uint8_t flags, api_errno *err);
int msc_std_close(int desc, api_errno *err);
std_rw_result msc_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err);
std_rw_result msc_std_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err);
int msc_std_lseek(int desc, int8_t whence, int32_t offset, int32_t *pos, api_errno *err);
int msc_std_sync(int desc, api_errno *err);

#endif /* _RIA_USB_MSC_H_ */
