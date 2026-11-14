/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* core/term/cells.h on the RIA, which has no screen. Its terminal follows the
 * VGA's: it is fed the bytes the VGA's terminal gets as they are sent, and
 * ria/sys/vga.c replays the VGA's resets and size changes in the same order.
 * It answers the program's queries, and nothing is drawn. */

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
