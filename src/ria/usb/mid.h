/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_USB_MID_H_
#define _RIA_USB_MID_H_

/* USB MIDI
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "api/api.h"
#include "api/std.h"

/* Main events
 */

void mid_task(void);
void mid_stop(void);

/* Status
 */

int mid_status_count(void);
int mid_status_response(char *buf, size_t buf_size, int state, unsigned width);

/* STDIO
 */

bool mid_std_handles(const char *name);
int mid_std_open(const char *name, uint8_t flags, api_errno *err);
std_rw_result mid_std_close(int desc, api_errno *err);
std_rw_result mid_std_sync(int desc, api_errno *err);
std_rw_result mid_std_read(int desc, char *buf, uint32_t buf_size, uint32_t *bytes_read, api_errno *err);
std_rw_result mid_std_write(int desc, const char *buf, uint32_t buf_size, uint32_t *bytes_written, api_errno *err);

#endif /* _RIA_USB_MID_H_ */
