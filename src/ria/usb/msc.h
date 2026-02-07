/*
 * Copyright (c) 2025 Rumbledethumps
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

/* Status
 */

int msc_status_count(void);
int msc_status_response(char *buf, size_t buf_size, int state);

/* STDIO
 */

bool msc_std_handles(const char *path);
int msc_std_open(const char *path, uint8_t flags);
bool msc_std_close(int desc_idx);
int msc_std_read(int desc_idx, char *buf, int count);
int msc_std_write(int desc_idx, const char *buf, int count);
int32_t msc_std_lseek(int desc_idx, int8_t whence, int32_t offset);
bool msc_std_sync(int desc_idx);

#endif /* _RIA_USB_MSC_H_ */
