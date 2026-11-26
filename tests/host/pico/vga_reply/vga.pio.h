/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The program pioasm makes of src/host/pico/vga/vga.pio that the VGA's ria.c
 * loads, as a name alone: no state machine runs on the host. */

#ifndef _TEST_VGA_PIO_H_
#define _TEST_VGA_PIO_H_

#include <hardware/pio.h>

extern const pio_program_t ria_backchannel_tx_program;

static inline pio_sm_config ria_backchannel_tx_program_get_default_config(uint offset)
{
    (void)offset;
    return (pio_sm_config){0};
}

#endif /* _TEST_VGA_PIO_H_ */
