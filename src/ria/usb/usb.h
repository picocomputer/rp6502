/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _DEV_H_
#define _DEV_H_

#include <stdint.h>

void usb_print_status();
void usb_set_status(uint8_t dev_addr, const char *format, ...);

#endif /* _DEV_H_ */
