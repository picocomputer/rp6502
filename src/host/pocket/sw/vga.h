/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_VGA_H_
#define _HOST_POCKET_SW_VGA_H_

#include "core/sys/sst.h"
#include "core/vga/vga.h"

#include <stdint.h>
#include <stdbool.h>

/* vid.c's savestate row carries this file's record of the scanline
 * table. */
#define VGA_TABLE_SST_SIZE (2 + VGA_PROG_MAX / 32 * 4 + 2)
void vga_table_save(sst_cursor_t *c);
bool vga_table_load(sst_cursor_t *c);

#endif /* _HOST_POCKET_SW_VGA_H_ */
