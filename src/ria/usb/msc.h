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
#include "api/api.h"
#include "api/std.h"

/* Status
 */

void msc_task(void);
int msc_status_count(void);
int msc_status_response(char *buf, size_t buf_size, int state);

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
