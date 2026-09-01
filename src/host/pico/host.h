/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_PICO_HOST_H_
#define _HOST_PICO_HOST_H_

/* The SDK. It enters the build here and nowhere else: a core file asks
 * this host for what it needs, rather than reaching for headers only this
 * host has. */
#include <pico.h>
#include <pico/stdlib.h>

/* This machine means all of it. Defined before osal/os.h, which supplies
 * the do-nothing answers every other machine gives. */
#define HOST_IN_FLASH(group) __in_flash(group)
#define HOST_NOT_IN_FLASH(group) __not_in_flash(group)
#define HOST_UNINITIALIZED_RAM(name) __uninitialized_ram(name)
#define HOST_TIME_CRITICAL(name) __time_critical_func(name)
#define HOST_ISR __isr

/* The SXGA console's two extra rows. The SIO interpolators mode4 walks are
 * not named here: PICO_ON_DEVICE already says which machine has them, and
 * only the VGA firmware links hardware_interp, so the one file that uses
 * them includes the header itself. */
#define HOST_TERM_MAX_HEIGHT 32

/* The launcher chain's path buffer, before the contract's default. */
#define PROC_PATH_MAX 256

#include "osal/os.h"

#endif /* _HOST_PICO_HOST_H_ */
