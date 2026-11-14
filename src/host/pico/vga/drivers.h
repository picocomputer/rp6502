/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_DRIVERS_H_
#define _HOST_DRIVERS_H_

#include "core/sys/driver.h"

#include "core/term/font.h"
#include "core/term/cells.h"
#include "vga/com/manifold.h"
#include "vga/sys/led.h"
#include "vga/sys/pix.h"
#include "vga/sys/ria.h"
#include "vga/sys/flash.h"
#include "vga/sys/vga.h"
#include "vga/usb/cdc.h"
#include "vga/usb/usb.h"

/* MANIFOLD_UART comes first because manifold_init installs its stdio driver,
 * and any later driver may print. CELLS comes before VGA because vga_init
 * programs the console canvas through mode0_prog, which sets the terminal's
 * height. VGA comes before PIX because scanvideo claims DMA channels 0 to 4
 * with dma_claim_mask, which panics if pix_init has already claimed one of
 * them. */
#define RP6502_MACH_DRIVERS                                              \
    MANIFOLD_UART_DRIVER, RIA_DRIVER, CELLS_DRIVER, FONT_DRIVER,          \
    VGA_DRIVER, CDC_DRIVER, USB_DRIVER, LED_DRIVER, PIX_DRIVER,          \
    FLASH_DRIVER

#endif /* _HOST_DRIVERS_H_ */
