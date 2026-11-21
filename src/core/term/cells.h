/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_TERM_CELLS_H_
#define _CORE_TERM_CELLS_H_

/* The console's screen on a machine that draws it: the console's terminal
 * (console_term.h) over mode 0's cells, fed every byte of the console, the
 * blinks of both sizes, the drawing of the console's one smooth scroll, and
 * the save state of all of it. It writes what mode 0 draws
 * (core/vga/mode/mode0.h) for the size showing. */

#include "core/sys/sst.h"
#include "core/term/term.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void cells_init(void);
void cells_task(void);

/* The console's terminal and the cells of both sizes as at power-up
 * (console_init). It is the RESET hook, because a load that fails leaves
 * any value in all of them. The renderer's RESET, which runs first, leaves
 * setting the heights again to its next task. */
void cells_restart(void);

/* Writes mode 0's words for the size showing. cells_out_chars calls it
 * after each batch of the console's bytes, and cells_task after the blinks,
 * a reset, a new height or another size showing. Something else that
 * changes what shows calls it too: a savestate load that puts the canvas
 * back, or a test that feeds term_write directly. */
void cells_publish(void);

/* RIS and the system's soft reset (console_term_reset): the blinks start
 * again, and everything is published. A machine that draws nothing has it
 * do nothing. */
void draw_reset(void);

/* The chunk holds the console's terminal (term.h) and both sizes in full:
 * each one's grid, the layout of its screens' cells (the scroll remap and
 * the fills its rows are owed) and its blinks, then the smooth scroll and
 * the palette (color.h). The cells and their colors are MODE0_DRIVER's
 * (mode0.h), which follows this one in the driver list. */
extern const sst_table_t cells_sst;

#define CELLS_DRIVER DRIVER(                                    \
    INIT(cells_init)                                            \
    TASK_FATFS(cells_task)                                      \
    RESET(cells_restart)                                        \
    SST("CELL", cells))

#endif /* _CORE_TERM_CELLS_H_ */
