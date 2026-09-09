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

/* The block, the finer counters behind it, and the fractions a host's motion
 * leaves over: 2 + 5 + 2 + 2 + 4 + 4. */
#define MOUSE_SST_SIZE 19
void mouse_sst_save(sst_cursor_t *c, unsigned flags);
bool mouse_sst_load(sst_cursor_t *c, unsigned flags);

#define MOUSE_DRIVER DRIVER(mouse_init, nul_task, nul_task, nul_run, mouse_stop, nul_break, \
    nul_config, nul_config, SST(MOUS, 1, MOUSE_SST_SIZE, mouse_sst_save, mouse_sst_load))

#endif /* _CORE_HID_MOUSE_H_ */
