/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TEST_XCHECK_MACHINE_H_
#define _TEST_XCHECK_MACHINE_H_

/* The Pico VGA's console terminal on the host, for xcheck: 32 rows and the
 * alternate screen, as the RIA's too. Nothing goes in flash. */

#define HOST_IN_FLASH
#define HOST_IN_FLASH_FUNC __attribute__((noinline))
#define HOST_IN_RAM
#define HOST_IN_RAM_FUNC
#define HOST_ISR_FUNC

#define TERM_MAX_HEIGHT 32
#define TERM_ALT_SCREEN 1

#endif /* _TEST_XCHECK_MACHINE_H_ */
