/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _HOST_HOST_H_
#define _HOST_HOST_H_

#include <stdbool.h>
#include <stdint.h>

#ifndef HOST_IN_FLASH
#define HOST_IN_FLASH(group)
#endif
#ifndef HOST_NOT_IN_FLASH
#define HOST_NOT_IN_FLASH(group)
#endif
#ifndef HOST_UNINITIALIZED_RAM
#define HOST_UNINITIALIZED_RAM(name) name
#endif
#ifndef HOST_TIME_CRITICAL
#define HOST_TIME_CRITICAL(name) name
#endif
#ifndef HOST_ISR
#define HOST_ISR
#endif

#ifndef HOST_TERM_MAX_HEIGHT
#define HOST_TERM_MAX_HEIGHT 30
#endif

#ifndef PROC_PATH_MAX
#define PROC_PATH_MAX 4096
#endif

#ifndef COM_RING_SIZE
#define COM_RING_SIZE 256
#endif

/* This random seed is constant for the entire run. */
uint32_t host_random_seed(void);
uint64_t host_clock_us(void);

#endif /* _HOST_HOST_H_ */
