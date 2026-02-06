/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_USB_CDC_H_
#define _RIA_USB_CDC_H_

/* USB CDC ACM (Communications Device Class - Abstract Control Model)
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void cdc_task(void);


int cdc_open(const char* name); // start at COM0: etc. 
bool cdc_close(int desc_idx); // start at COM0: etc.
int cdc_rx(char *buf, int buf_size);
int cdc_tx(const char *buf, int buf_size);

#endif /* _RIA_USB_CDC_H_ */
