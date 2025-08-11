/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _KBD_H_
#define _KBD_H_

/* Kernel events
 */

void kbd_init(void);
void kbd_task(void);
void kbd_stop(void);

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

#endif /* _KBD_H_ */
