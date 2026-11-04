/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_MACHINE_H_
#define _HOST_MACHINE_H_

#include <pico.h>

/* Code fetched through the XIP cache takes the bandwidth the 6502 bus needs,
 * so code in flash runs only while RESB is low. A function the compiler
 * inlines lands where its caller is, so HOST_IN_FLASH_FUNC is never inlined. */
#define HOST_IN_FLASH(group) __in_flash(group)
#define HOST_IN_FLASH_FUNC(group) __noinline __in_flash(group)
#define HOST_NOT_IN_FLASH(group) __not_in_flash(group)
#define HOST_UNINITIALIZED_RAM(name) __uninitialized_ram(name)

#define XRAM_ALIGN 4
#define HID_MAX_SLOTS 16


#endif /* _HOST_MACHINE_H_ */
