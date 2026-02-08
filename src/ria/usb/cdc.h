/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_USB_CDC_H_
#define _RIA_USB_CDC_H_

/* USB CDC ACM (Communications Device Class - Abstract Control Model)
 * Host-mode driver for USB serial adapters.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void cdc_task(void);

/* Status
 */

int cdc_status_count(void);
int cdc_status_response(char *buf, size_t buf_size, int state);

/* STDIO
 */

bool cdc_std_handles(const char *name);
int cdc_std_open(const char *name, uint8_t flags);
bool cdc_std_close(int desc_idx);
int cdc_std_read(int desc_idx, char *buf, uint32_t buf_size, uint32_t *bytes_read);
int cdc_std_write(int desc_idx, const char *buf, uint32_t buf_size, uint32_t *bytes_written);

#endif /* _RIA_USB_CDC_H_ */
