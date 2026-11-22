/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The Pico SDK's interrupt masking. test_vga.c runs a handler only between
 * the calls it makes, so there is nothing to mask. */

#ifndef _TEST_HARDWARE_SYNC_H_
#define _TEST_HARDWARE_SYNC_H_

#include <stdint.h>

static inline uint32_t save_and_disable_interrupts(void) { return 0; }
static inline void restore_interrupts(uint32_t status) { (void)status; }

#endif /* _TEST_HARDWARE_SYNC_H_ */
