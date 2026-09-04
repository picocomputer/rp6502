/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_VGA_H_
#define _HOST_POCKET_SW_VGA_H_

#include "core/vga/vga.h"

#include <stdint.h>
#include <stdbool.h>

/* Both sides of the VGA contract on one machine, the core/vga/vga.h
 * shape. What only the fabric has.
 */

int16_t vga_vsync_scanline(void);

bool vga_prog_valid(int16_t plane, int16_t scanline_begin,
                    int16_t *scanline_end);
/* Mode 0's registration: one instance globally — the previous entries
 * are swept wherever they survive, contiguous or not. */
/* fill_fn is the renderer itself on a machine that renders in software.
 * Here the fabric does, told which mode by vga_mode_begin before the call,
 * so it is ignored -- but it stays in the signature because core/vga/mode/mode0.c
 * passes it and term.c drags the five-parameter declaration into this same
 * link through core/vga/vga.h. */
bool vga_prog_exclusive(int16_t plane, int16_t scanline_begin,
                        int16_t scanline_end, uint16_t config_ptr,
                        bool (*fill_fn)(int16_t, int16_t, int16_t,
                                        uint16_t *, uint16_t));
/* The two raster registers a blob cannot carry, put back from the
 * shadows it did. */
void vga_restore(void);

#endif /* _HOST_POCKET_SW_VGA_H_ */
