/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RTL_SW_SHIM_HARDWARE_PIO_H_
#define _RTL_SW_SHIM_HARDWARE_PIO_H_

#include <assert.h>
#include <stdint.h>

typedef struct
{
    volatile uint32_t txf[4];
} pio_hw_t;

extern pio_hw_t *const pio1;

static inline unsigned pio_sm_get_tx_fifo_level(pio_hw_t *pio, unsigned sm)
{
    (void)pio;
    (void)sm;
    return 0;
}

static inline void pio_sm_put(pio_hw_t *pio, unsigned sm, uint32_t msg)
{
    (void)pio;
    (void)sm;
    (void)msg;
}

#define tight_loop_contents() ((void)0)

#endif
