/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/vga/vga.h"
#include "core/vga/mode/mode0.h"
#include "core/vga/mode/mode1.h"
#include "core/vga/mode/mode2.h"
#include "core/vga/mode/mode3.h"
#include "core/vga/mode/mode4.h"
#include "core/vga/mode/mode5.h"

bool vga_mode_prog(uint16_t mode, uint16_t *xregs)
{
    switch (mode)
    {
    case 0:
        return mode0_prog(xregs);
    case 1:
        return mode1_prog(xregs);
    case 2:
        return mode2_prog(xregs);
    case 3:
        return mode3_prog(xregs);
    case 4:
        return mode4_prog(xregs);
    case 5:
        return mode5_prog(xregs);
    default:
        return false;
    }
}
