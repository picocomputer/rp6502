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

/* What vga.c keeps of the fabric's scanline table. It loads after the
 * canvas, which VID_VSYNC_LINE is computed from. */
extern const sst_table_t vga_fabric_sst;

#define VGA_FABRIC_DRIVER DRIVER(                              \
    SST("VGA_", vga_fabric))

#endif /* _HOST_POCKET_SW_VGA_H_ */
