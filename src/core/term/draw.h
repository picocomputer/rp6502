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

/* DRAW_PUBLISH_PER_FRAME: draw_task writes mode 0's words at most once a
 * frame, and the console's bytes only mark them for it. For a machine whose
 * mode 0 reads the words as it draws each scanline, with no time to spare:
 * it builds a row again whenever the row's word changes, and a scroll
 * changes every row's word, so words written after every byte of a burst
 * of scrolls make it build rows again on most scanlines and lines come out
 * late. Nothing is lost, since a frame shows one picture. 0, the default,
 * writes them after each batch of the console's bytes. */
#ifndef DRAW_PUBLISH_PER_FRAME
#define DRAW_PUBLISH_PER_FRAME 0
#endif

void draw_init(void);
void draw_task(void);

/* The console's terminal and the cells of both sizes as at power-up
 * (vterm_init). It is the RESET hook, because a load that fails leaves
 * any value in all of them. The renderer's RESET, which runs first, leaves
 * setting the heights again to its next task. */
void draw_restart(void);

/* Writes mode 0's words for the size showing, the selection's columns of
 * each row in its row word while the selection shows on the screen
 * showing. The ghost row of a smooth scroll and the spare row are never
 * the selection's (screen.h), so smooth_ghost carries none.
 * draw_out_chars calls it after each batch of the console's bytes (with
 * DRAW_PUBLISH_PER_FRAME, draw_task does), and draw_task after the blinks,
 * a reset, a new height, another size showing or a change of the
 * selection, which can come with no byte of the console: the mouse's, or
 * the RIA's sent to the VGA. Something else that changes what shows calls
 * it too: a savestate load that puts the canvas back, or a test that feeds
 * term_write directly. */
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

/* The console's terminal started a job (screen.h): a byte or a change of
 * the console's size or code page works on more than one row. A machine
 * that draws the console does its own jobs, so it has it do nothing. The
 * RIA, whose VGA does the same job a few rows a pass of its main loop,
 * holds its console bytes for the VGA a while (ria/sys/vga.c). */
void draw_job_started(void);

/* The chunk holds the console's terminal (term.h) and both sizes in full:
 * each one's grid, where mode 0 sits (vterm.h), the layout of its screens'
 * cells (the scroll remap), the selection (screen.h) and its blinks, then
 * the smooth scroll and the palette (color.h). The cells and their colors
 * are MODE0_DRIVER's (mode0.h), which follows this one in the driver
 * list. */
extern const sst_table_t draw_sst;

#define DRAW_DRIVER DRIVER(                                    \
    INIT(draw_init)                                            \
    TASK_FATFS(draw_task)                                      \
    RESET(draw_restart)                                        \
    SST("CELL", draw))

#endif /* _CORE_TERM_DRAW_H_ */
