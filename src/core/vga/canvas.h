/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_VGA_CANVAS_H_
#define _CORE_VGA_CANVAS_H_

/* The canvas and the console's size on a machine that draws its own console
 * (canvas.c); vga.h has its calls. */

#include "core/sys/sst.h"

extern const sst_table_t canvas_sst;

/* CANVAS_DRIVER follows CELLS_DRIVER in the driver list, because its load
 * publishes the cells for the canvas it puts back. */
void canvas_reset(void);

#define CANVAS_DRIVER DRIVER(                                  \
    RESET(canvas_reset)                                        \
    SST("CANV", canvas))

#endif /* _CORE_VGA_CANVAS_H_ */
