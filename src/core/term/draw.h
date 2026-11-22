/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_TERM_DRAW_H_
#define _CORE_TERM_DRAW_H_

/* The console's screen on a machine that draws it: the console's terminal
 * (vterm.h) over mode 0's cells, fed every byte of the console, the
 * blinks of both sizes, the drawing of the console's one smooth scroll, and
 * the save state of all of it. It writes what mode 0 draws
 * (core/vga/mode/mode0.h) for the size showing. On a machine with
 * TERM_JOB_ROWS (term.h), draw_task does that many rows of the jobs
 * pending each pass of the main loop. */

#include "core/sys/sst.h"
#include "core/term/term.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void draw_init(void);
void draw_task(void);

/* The console's terminal and the cells of both sizes as at power-up
 * (vterm_init). It is the RESET hook, because a load that fails leaves
 * any value in all of them. The renderer's RESET, which runs first, leaves
 * setting the heights again to its next task. */
void draw_restart(void);

/* Writes mode 0's words for the size showing. draw_out_chars calls it
 * after each batch of the console's bytes, and draw_task after the blinks,
 * a reset, a new height or another size showing. Something else that
 * changes what shows calls it too: a savestate load that puts the canvas
 * back, or a test that feeds term_write directly. */
void draw_publish(void);

/* The console's bytes that came while a job was pending (TERM_JOB_ROWS),
 * each of which finished the jobs at once before it went in. The RIA holds
 * its bytes back while the VGA works, so on the Pico VGA this stays 0. The
 * hardware tests (tests/hil) read it and clear it through the VGA's debug
 * probe. */
extern volatile uint32_t draw_bytes_in_jobs;

/* RIS and the system's soft reset (vterm_reset): the blinks start
 * again, and everything is published. A machine that draws nothing has it
 * do nothing. */
void draw_reset(void);

/* The chunk holds the console's terminal (term.h) and both sizes in full:
 * each one's grid, where mode 0 sits (vterm.h), the layout of its screens'
 * cells (the scroll remap) and its blinks, then the smooth scroll and the
 * palette (color.h). The cells and their colors are MODE0_DRIVER's
 * (mode0.h), which follows this one in the driver list. */
extern const sst_table_t draw_sst;

#define DRAW_DRIVER DRIVER(                                    \
    INIT(draw_init)                                            \
    TASK_FATFS(draw_task)                                      \
    RESET(draw_restart)                                        \
    SST("CELL", draw))

#endif /* _CORE_TERM_DRAW_H_ */
