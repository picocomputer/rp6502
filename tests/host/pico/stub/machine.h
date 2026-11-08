/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TEST_PICO_MACHINE_H_
#define _TEST_PICO_MACHINE_H_

/* The RIA's machine.h for a host build of its sources: nothing goes in flash. */

#define __in_flash(group)
#define HOST_IN_FLASH(group)
#define HOST_IN_FLASH_FUNC(group) __attribute__((noinline))
#define HOST_NOT_IN_FLASH(group)
#define HOST_UNINITIALIZED_RAM(name) name

#define XRAM_ALIGN 4
#define HID_MAX_SLOTS 16

#endif /* _TEST_PICO_MACHINE_H_ */
