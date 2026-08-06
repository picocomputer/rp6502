/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Soft-CPU shim for <hardware/pio.h>, shadowing the emulator's shim via
 * include-path precedence. This machine has no PIX bus — the video device
 * shares the fabric — so the TX FIFO reads permanently full and a put goes
 * nowhere. pio1 is never loaded: every entry point here ignores it.
 */

#ifndef _RTL_SW_SHIM_HARDWARE_PIO_H_
#define _RTL_SW_SHIM_HARDWARE_PIO_H_

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
    return 8;
}

static inline void pio_sm_put(pio_hw_t *pio, unsigned sm, uint32_t msg)
{
    (void)pio;
    (void)sm;
    (void)msg;
}

#define tight_loop_contents() ((void)0)

#endif /* _RTL_SW_SHIM_HARDWARE_PIO_H_ */
