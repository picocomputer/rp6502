/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_HID_MOUSE_H_
#define _CORE_HID_MOUSE_H_

#include <stddef.h>
#include <stdint.h>
#include "core/sys/sst.h"
#include <stdbool.h>

#include "core/hid/hid.h"

#define MOUSE_MAX_MICE 4

typedef struct mouse_connection
{
    bool valid;
    int slot;
    uint8_t report_id; // when non-zero, the report starts with this byte
    uint16_t button_offsets[8];
    bool x_relative;
    uint16_t x_offset;
    uint8_t x_size;
    uint16_t y_offset;
    uint8_t y_size;
    uint16_t wheel_offset;
    uint8_t wheel_size;
    uint16_t pan_offset; // horizontal scroll
    uint8_t pan_size;
    uint8_t buttons;
} mouse_connection_t;

void mouse_init(void);
/* Unmaps the block. It is also the RESET hook: a failed load can leave the
 * address pointing into the next program's XRAM, and the counters behind the
 * block are deltas that any value serves. */
void mouse_stop(void);

bool mouse_xreg(uint16_t word);

bool mouse_is_mapped(void);

/* Host motion in Q16.16 counts, where 65536 is one count of the block's x and
 * y and the fraction is carried between calls. Fixed point rather than float
 * because a savestate carries these counters and netplay compares two
 * machines' blobs byte for byte. */
#define MOUSE_ONE 65536
void mouse_host_move(int32_t dx, int32_t dy);

/* The wheel and pan bytes are counters a program reads by subtracting the
 * value it saw last, so these add to them and wrap. */
void mouse_host_wheel(int dwheel, int dpan);

/* Buttons in HID order: bit 0 left, bit 1 right, bit 2 middle. */
void mouse_host_buttons(uint8_t buttons);

bool mouse_mount(int slot, const mouse_connection_t *desc);

bool mouse_umount(int slot);

void mouse_report(int slot, uint8_t const *report, size_t size);

extern const sst_table_t mouse_sst;

#define MOUSE_DRIVER DRIVER(                                      \
    INIT(mouse_init)                                              \
    STOP(mouse_stop)                                              \
    RESET(mouse_stop)                                             \
    SST("MOUS", mouse))

#endif /* _CORE_HID_MOUSE_H_ */
