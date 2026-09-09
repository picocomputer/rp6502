/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* PIX XREG register dispatch. Device 0 is the RIA's own human interface and
 * audio registers, and device 1 is the video device.
 *
 * Device 0 never crosses a bus, so every machine that runs the API compiles
 * core/api/xreg0.c. Device 1 is compiled only into a machine that renders its
 * own video; a machine whose video device is a real chip across the PIX bus
 * answers device 1 at the far end instead, in host/pico/vga/sys/pix.c.
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

/* Sixteen staging registers of two bytes. A program sets its parameters one
 * register at a time and the CANVAS or MODE write consumes the lot and clears
 * them, so a save taken between the two holds a half-assembled mode program
 * that lives nowhere else. */
#define XREG_SST_SIZE 32
void xreg_sst_save(sst_cursor_t *c, unsigned flags);
bool xreg_sst_load(sst_cursor_t *c, unsigned flags);

#define XREG_DRIVER DRIVER(nul_init, nul_task, nul_task, nul_run, nul_stop, nul_break, \
    nul_config, nul_config, SST(XREG, 1, XREG_SST_SIZE, xreg_sst_save, xreg_sst_load))

#endif /* _CORE_API_XREG_H_ */
