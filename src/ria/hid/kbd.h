/*
 * Copyright (c) 2026 Rumbledethumps
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

/* Main events
 */

void kbd_init(void);
void kbd_task(void);
void kbd_stop(void);

// Responder prints all keyboard layout options.
int kbd_layouts_response(char *buf, size_t buf_size, int state, unsigned width);

// Called when code page changes so cache can be rebuilt.
void kbd_rebuild_code_page_cache(void);

// Attempt to mount this HID descriptor
bool kbd_mount(int slot, uint8_t const *desc_data, uint16_t desc_len,
               uint16_t vendor_id, uint16_t product_id);

// Clean up descriptor when device is disconnected.
bool kbd_umount(int slot);

// Process HID keyboard report.
void kbd_report(int slot, uint8_t const *data, size_t size);

// Set the extended register value.
bool kbd_xreg(uint16_t word);

// Drain the keyboard queue into buf
size_t kbd_stdio_in_chars(char *buf, size_t length);

// Configuration setting KB
#define KBD_LAYOUT_LIST_SIZE 40
void kbd_load_layout(const char *str);
bool kbd_set_layout(const char *list);
const char *kbd_get_layout_list(void);
const char *kbd_get_layout(void);
const char *kbd_get_layout_verbose(void);

#endif /* _RIA_HID_KBD_H_ */
