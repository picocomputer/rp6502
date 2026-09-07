/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* PIX XREG register dispatch: device 0 is the RIA's own HID and audio,
 * device 1 the video device. Both are the 6502's ABI rather than anything a
 * machine chooses, which is why they are here and not a root contract -- no
 * machine answers them, they answer for every machine.
 *
 * Device 0 never crosses a bus, so it is compiled into all of them. Device 1
 * is compiled only into a machine that is its own video; one with a real bus
 * sends the message and the far end answers (host/pico/vga/sys/pix.c).
 */

#ifndef _CORE_API_XREG_H_
#define _CORE_API_XREG_H_

#include "core/sys/driver.h"
#include "core/sys/sst.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    bool xreg0(uint8_t channel, uint8_t address, uint16_t word);
    bool xreg1(uint8_t channel, uint8_t address, uint16_t word);

#ifdef __cplusplus
}
#endif

/* The staging registers a mode write consumes. A program sets its parameters
 * one xreg at a time and the MODE write reads the lot, so a save taken
 * between the two has a half-assembled program that lives nowhere else --
 * the write clears them the moment it has them. */
#define XREG_SST_SIZE 32
void xreg_sst_save(sst_cursor_t *c, unsigned flags);
bool xreg_sst_load(sst_cursor_t *c, unsigned flags);

#define XREG_DRIVER DRIVER(nul_init, nul_task, nul_task, nul_run, nul_stop, nul_break, \
    nul_config, nul_config, SST(XREG, 1, XREG_SST_SIZE, xreg_sst_save, xreg_sst_load))

#endif /* _CORE_API_XREG_H_ */
