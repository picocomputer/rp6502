/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The Pico SDK's spin locks. test_vga_reply.c runs one core, so a lock is
 * always free. */

#ifndef _TEST_HARDWARE_SYNC_H_
#define _TEST_HARDWARE_SYNC_H_

#include <stdbool.h>
#include <stdint.h>

typedef volatile uint32_t spin_lock_t;
static inline unsigned int spin_lock_claim_unused(bool required) { (void)required; return 0; }
static inline spin_lock_t *spin_lock_init(unsigned int num)
{
    static spin_lock_t lock;
    (void)num;
    return &lock;
}
static inline uint32_t spin_lock_blocking(spin_lock_t *lock) { (void)lock; return 0; }
static inline void spin_unlock(spin_lock_t *lock, uint32_t saved) { (void)lock; (void)saved; }

#endif /* _TEST_HARDWARE_SYNC_H_ */
