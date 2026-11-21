/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The console's terminal on the RIA, which has no screen (console_term.h):
 * it answers the program's queries, and nothing is drawn. This file feeds it
 * the console's bytes and keeps the cell store's functions that change
 * cells (screen.h), with no cells. */

#ifndef _RIA_TERM_CELLS_H_
#define _RIA_TERM_CELLS_H_

#include "core/term/cells.h"

void cells_flashing(void);

/* Before VGA_DRIVER, because vga_init resets and sizes the terminal as it
 * connects. While flashing, the console's output path runs from RAM and the
 * terminal, in flash, is no longer fed. */
#define CELLS_RIA_DRIVER DRIVER( \
    INIT(cells_init)             \
    FLASHING(cells_flashing))

#endif /* _RIA_TERM_CELLS_H_ */
