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

// Number of currently mounted CDC devices.
int cdc_count(void);

// For monitor status command.
int cdc_status_response(char *buf, size_t buf_size, int state);

// Open a CDC device by name (e.g. "COM0:").
// Returns descriptor index on success, -1 on failure.
int cdc_open(const char *name);

// Close a previously opened descriptor.
// Returns false if not open.
bool cdc_close(int desc_idx);

// Read from an open CDC descriptor.
// Returns number of bytes read, or -1 on error.
int cdc_rx(int desc_idx, char *buf, int buf_size);

// Write to an open CDC descriptor.
// Returns number of bytes written, or -1 on error.
int cdc_tx(int desc_idx, const char *buf, int buf_size);

#endif /* _RIA_USB_CDC_H_ */
