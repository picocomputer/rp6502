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

#define XRAM_ALIGN 4
#define HID_MAX_SLOTS 16

#endif /* _HOST_MACHINE_H_ */
