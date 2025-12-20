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

// For monitor status command.
int msc_count(void);
int msc_status_response(char *buf, size_t buf_size, int state);

#endif /* _RIA_USB_MSC_H_ */
