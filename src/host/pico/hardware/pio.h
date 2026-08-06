/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host shim for <hardware/pio.h>: just enough of the Pico PIO API for the shared
 * PIX driver (ria/sys/pix.h) to compile and run on the host, shadowing the SDK
 * header via the emu/host include root like the pico/* shims beside it. The PIX
 * bus is PIO1 SM1; with no real FIFO the emulator models a put as an immediate
 * receive — emu/sys/pix.c implements pio_sm_put and owns pio1.
 */

#ifndef _EMU_SHIM_HARDWARE_PIO_H_
#define _EMU_SHIM_HARDWARE_PIO_H_

#include <assert.h>
#include <stdint.h>

/* Opaque PIO block; the shim entry points ignore it (there is no FIFO). txf only
 * keeps the on-device PIX_SEND_XRAM macro well-formed — the emu never uses it. */
typedef struct
{
    volatile uint32_t txf[4];
} pio_hw_t;

extern pio_hw_t *const pio1; /* the PIX PIO (emu/sys/pix.c) */

/* The TX FIFO never fills in the emu, so a send is always ready. */
static inline unsigned pio_sm_get_tx_fifo_level(pio_hw_t *pio, unsigned sm)
{
    (void)pio;
    (void)sm;
    return 0;
}

/* A PIX message written to the bus — delivered immediately (emu/sys/pix.c). */
void pio_sm_put(pio_hw_t *pio, unsigned sm, uint32_t msg);

#define tight_loop_contents() ((void)0)

#endif /* _EMU_SHIM_HARDWARE_PIO_H_ */
