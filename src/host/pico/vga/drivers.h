/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This machine's drivers: the ones it is made of and the order it comes up
 * in. There is no 6502 here -- this firmware is the video half of a
 * Picocomputer, driven over PIX -- so the rows carry init and task and
 * nothing else, and src/host/pico/vga/main.c walks them.
 *
 * No stdio table either: what a program may open is the RIA's business.
 */

#ifndef _HOST_DRIVERS_H_
#define _HOST_DRIVERS_H_

#include "core/sys/driver.h"

#include "core/term/font.h"
#include "core/term/term.h"
#include "vga/sys/com.h"
#include "vga/sys/led.h"
#include "vga/sys/pix.h"
#include "vga/sys/ria.h"
#include "vga/sys/flash.h"
#include "vga/sys/vga.h"
#include "vga/usb/cdc.h"
#include "vga/usb/usb.h"

/* The order is the fabric's: the console first, because everything may print
 * and the backchannel borrows its RX pin; the terminal and its glyphs before
 * the beam that asks them what to draw; the beam before PIX, whose first
 * xreg can reach any of them. CDC before USB, the order com_out_chars pumps
 * them in when it has to drain by hand. Each row's header says the rest.
 *
 * Core 1 is launched after this walk, not inside it -- see main.c. */
#define RP6502_MACH_DRIVERS                                              \
    COM_DRIVER, RIA_DRIVER, TERM_DRIVER, FONT_DRIVER, VGA_DRIVER,        \
    CDC_DRIVER, USB_DRIVER, LED_DRIVER, PIX_DRIVER, FLASH_DRIVER

#endif /* _HOST_DRIVERS_H_ */
