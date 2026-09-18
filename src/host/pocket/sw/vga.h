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

bool vga_prog_exclusive(int16_t plane, int16_t scanline_begin,
                        int16_t scanline_end, uint16_t config_ptr,
                        bool (*fill_fn)(int16_t, int16_t, int16_t,
                                        uint16_t *, uint16_t));
void vga_restore(void);

#endif /* _HOST_POCKET_SW_VGA_H_ */
