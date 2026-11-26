/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The programs pioasm makes of src/host/pico/ria/ria.pio that pix.c and
 * vga.c load, as names alone: no state machine runs on the host. */

#ifndef _TEST_RIA_PIO_H_
#define _TEST_RIA_PIO_H_

#include <hardware/pio.h>

extern const pio_program_t pix_tx_program;
extern const pio_program_t vga_backchannel_rx_program;
#define pix_tx_offset_token 0
#define pix_tx_offset_check 0

static inline pio_sm_config pix_tx_program_get_default_config(uint offset)
{
    (void)offset;
    return (pio_sm_config){0};
}

static inline pio_sm_config vga_backchannel_rx_program_get_default_config(uint offset)
{
    (void)offset;
    return (pio_sm_config){0};
}

#endif /* _TEST_RIA_PIO_H_ */
