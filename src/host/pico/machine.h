/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_PICO_MACHINE_H_
#define _HOST_PICO_MACHINE_H_

/* The SDK's own prelude, and it has to be all of it. Narrowing this to the two
 * headers that spell the attributes below moved 560 bytes of RIA-W: pico.h is
 * the config every later SDK header reads, and without it hardware/pio.h picks
 * different functions -- pio_sm_set_pindirs_with_mask became the mask64 one.
 * Measured, not assumed. */
#include <pico.h>
#include <pico/stdlib.h>

/* This machine means all of them. Said before host/host.h, which supplies the
 * do-nothing answers every other machine gives. */
#define HOST_IN_FLASH(group) __in_flash(group)
#define HOST_NOT_IN_FLASH(group) __not_in_flash(group)
#define HOST_UNINITIALIZED_RAM(name) __uninitialized_ram(name)

/* The SXGA console's two extra rows. The SIO interpolators mode4 walks are
 * not named here: PICO_ON_DEVICE already says which machine has them, and
 * only the VGA firmware links hardware_interp, so the one file that uses
 * them includes the header itself. */
#define HOST_TERM_MAX_HEIGHT 32

/* The launcher chain's path buffer, before the contract's default. */
#define PROC_PATH_MAX 256

#include "host/host.h"

#endif /* _HOST_PICO_MACHINE_H_ */
