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
#include "core/api/api.h"
#include "core/api/std.h"

/* Main events
 */

void mid_task(void);
void mid_stop(void);

/* Status
 */

int mid_status_response(char *buf, size_t buf_size, int state, unsigned width);

/* STDIO
 */

bool mid_std_handles(const char *name);
int mid_std_open(const char *name, uint8_t flags, api_errno *err);
std_rw_result mid_std_close(int desc, api_errno *err);
std_rw_result mid_std_sync(int desc, api_errno *err);
std_rw_result mid_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err);
std_rw_result mid_std_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err);

/* This driver's row in a machine's driver list; see core/sys/driver.h. */
#define MID_DRIVER DRIVER(nul_init, mid_task, nul_task, nul_run, mid_stop, nul_break, nul_config, nul_config)

/* This driver's stdio row: the std_driver_t initializer core/api/std.c
 * builds this machine's table from. A stream that can be flushed, but not sought. */
#define MID_STD_DRIVER           \
    {                               \
        .handles = mid_std_handles, \
        .open = mid_std_open,       \
        .close = mid_std_close,     \
        .read = mid_std_read,       \
        .write = mid_std_write,     \
        .sync = mid_std_sync,       \
    }

#endif /* _RIA_USB_MID_H_ */
