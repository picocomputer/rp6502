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

void cdc_init(void);
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

// Line coding: baud rate, data bits, stop bits, parity.
// stop_bits: 0=1, 1=1.5, 2=2
// parity: 0=none, 1=odd, 2=even, 3=mark, 4=space
// data_bits: 5, 6, 7, 8
bool cdc_set_baudrate(int desc_idx, uint32_t baudrate);
bool cdc_set_data_format(int desc_idx, uint8_t stop_bits, uint8_t parity, uint8_t data_bits);

// Modem control lines.
// Note: tuh_cdc_connect/disconnect are called automatically in open/close,
// but manual control is available for applications that need it.
bool cdc_set_dtr(int desc_idx, bool state);
bool cdc_set_rts(int desc_idx, bool state);

#endif /* _RIA_USB_CDC_H_ */
