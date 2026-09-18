/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_MACHINE_H_
#define _HOST_MACHINE_H_

#include <pico.h>

#define HOST_IN_FLASH(group) __in_flash(group)
#define HOST_NOT_IN_FLASH(group) __not_in_flash(group)
#define HOST_UNINITIALIZED_RAM(name) __uninitialized_ram(name)

#define XRAM_ALIGN 4
#define HID_MAX_SLOTS 16


#endif /* _HOST_MACHINE_H_ */
