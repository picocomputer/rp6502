/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_HID_KBD_H_
#define _RIA_HID_KBD_H_

/* HID Keyboard driver
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* CONTRIBUTING: Keyboard layouts are easy to make and test.
 * There is plenty of flash memory so don't hesitate to submit
 * fully tested layouts.
 *
 * Duplicate and modify whatever header file gives you the best
 * start. Make sure you change the suffix of all the defines.
 * Then add the include here and an entry to KBD_LAYOUTS.
 *
 * You do not need debug hardware. You do not need to know C.
 * All you need to do is fill in the tables with unicode.
 * Use F7 to build then look in the build/src folder for the .uf2
 * file that you can load on a Pi Pico with a USB cable.
 */

#include "hid/kbd_de.h"
#include "hid/kbd_dk.h"
#include "hid/kbd_pl.h"
#include "hid/kbd_se.h"
#include "hid/kbd_us.h"

#define KBD_LAYOUT_MAX_NAME_SIZE 16
#define KBD_LAYOUTS                                \
    X(DK, "DK", "Denmark")                         \
    X(DE, "DE", "Germany")                         \
    X(PL_PROG, "PL-PROG", "Poland - Programmer's") \
    X(SE, "SE", "Sweden")                          \
    X(US, "US", "United States")                   \
    X(US_INTL, "US-INTL", "United States - International")

/* Main events
 */

void kbd_init(void);
void kbd_task(void);
void kbd_stop(void);

// Responder prints all keyboard layout options.
int kbd_layouts_response(char *buf, size_t buf_size, int state);

// Called when code page changes so cache can be rebuilt.
void kbd_rebuild_code_page_cache(void);

// Parse HID report descriptor
bool kbd_mount(int slot, uint8_t const *desc_data, uint16_t desc_len);

// Clean up descriptor when device is disconnected.
bool kbd_umount(int slot);

// Process HID keyboard report.
void kbd_report(int slot, uint8_t const *data, size_t size);

// Set the extended register value.
bool kbd_xreg(uint16_t word);

// Handler for stdio_driver_t
int kbd_stdio_in_chars(char *buf, int length);

// Configuration setting KB
void kbd_load_layout(const char *str, size_t len);
bool kbd_set_layout(const char *kb);
const char *kbd_get_layout(void);
const char *kbd_get_layout_verbose(void);

#endif /* _RIA_HID_KBD_H_ */
