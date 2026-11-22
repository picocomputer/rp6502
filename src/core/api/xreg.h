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

    bool xreg0(uint8_t channel, uint8_t index, uint16_t word);
    bool xreg1(uint8_t channel, uint8_t index, uint16_t word);

#ifdef __cplusplus
}
#endif

extern const sst_table_t xreg_sst;

/* Drops the staging registers, as a CANVAS or MODE write does after consuming
 * them. */
void xreg1_clear(void);

/* RESET drops a half-assembled mode program, so a failed savestate load
 * leaves nothing for the next program's MODE write to consume. */
#define XREG_DRIVER DRIVER(                                    \
    RESET(xreg1_clear)                                         \
    SST("XREG", xreg))

#endif /* _CORE_API_XREG_H_ */
