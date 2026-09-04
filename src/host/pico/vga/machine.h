/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What this machine says about itself: the plain facts a unit can learn
 * without including anything else. drivers.h beside it is what the machine
 * is made of. Nothing here has a default anywhere -- a unit that uses one of
 * these includes this file or does not compile.
 */

#ifndef _HOST_MACHINE_H_
#define _HOST_MACHINE_H_

/* The SDK's config, which the section attributes below are spelled in. */
#include <pico.h>

#define HOST_IN_FLASH(group) __in_flash(group)
#define HOST_NOT_IN_FLASH(group) __not_in_flash(group)
#define HOST_UNINITIALIZED_RAM(name) __uninitialized_ram(name)

/* The 6502's sixteen-bit wrap is folded into the pointer, so xram sits on a
 * 64 KB boundary here and nowhere else. */
#define XRAM_ALIGN 0x10000

/* The SXGA console: 512 scanlines over a 16-line font. */
#define TERM_MAX_HEIGHT 32

#endif /* _HOST_MACHINE_H_ */
