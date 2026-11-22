/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TEST_PICO_MACHINE_H_
#define _TEST_PICO_MACHINE_H_

/* The RIA's machine.h for a host build of its sources: nothing goes in flash. */

#define __in_flash(group)
#define HOST_IN_FLASH
#define HOST_IN_FLASH_FUNC __attribute__((noinline))
#define HOST_IN_RAM
#define HOST_IN_RAM_FUNC
#define HOST_ISR_FUNC
#define HOST_REGISTER_RAM(name) name

#define HID_MAX_SLOTS 16

#endif /* _TEST_PICO_MACHINE_H_ */
