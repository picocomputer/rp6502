/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_HID_TAB_H_
#define _RIA_HID_TAB_H_

/* HID absolute-pointer ("tablet") driver. Reports an absolute canvas position
 * instead of relative motion: a relative mouse is integrated and clamped to the
 * canvas, an absolute digitizer/pen is scaled to it. The multi-byte coordinates
 * are delivered coherently through byte-wide XRAM by a unary window encoding (no
 * RIA/act_loop help). See tab.c for the XRAM contract.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void tab_init(void);
void tab_stop(void);

// Set the extended register value.
bool tab_xreg(uint16_t word);

// Parse HID report descriptor for an absolute or relative pointer.
bool tab_mount(int slot, uint8_t const *desc_data, uint16_t desc_len);

// Clean up descriptor when device is disconnected.
bool tab_umount(int slot);

// Process HID report.
void tab_report(int slot, uint8_t const *report, size_t size);

#endif /* _RIA_HID_TAB_H_ */
