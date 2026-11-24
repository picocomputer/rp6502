/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/vga/mode/mode6.h"
#include "core/vga/vga.h"

/* The sprites are on a word, as mode 5's are. How many there are, and
 * whether each one's image fits, is checked as the pointer is drawn. */
bool mode6_prog(uint16_t *xregs)
{
    const uint16_t config_ptr = xregs[2];
    if (config_ptr != 0xFFFF && (config_ptr & 1))
        return false;
    return vga_prog_pointer(config_ptr);
}
