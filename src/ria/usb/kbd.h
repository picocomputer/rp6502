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

// Process HID keyboard report.
void kbd_report(uint8_t idx, void const *report, size_t size);

// Set the extended register value.
bool kbd_xreg(uint16_t word);

// Send LEDs to keyboards in next task.
void kbd_hid_leds_dirty();

// Handler for stdio_driver_t
int kbd_stdio_in_chars(char *buf, int length);

#endif /* _KBD_H_ */
