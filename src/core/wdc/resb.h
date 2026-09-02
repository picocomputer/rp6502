/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * RESB, the 6502's reset. One line, and it resets the 6522 beside it.
 *
 * Not a driver row, because no single position in a machine's list can be
 * both the first thing down and the last thing up, and because a stop asked
 * for before the machine ever started skips the fan-out entirely. So
 * core/sys/sys.c brackets the machine with it instead.
 */

#ifndef _CORE_WDC_RESB_H_
#define _CORE_WDC_RESB_H_

#include <stdbool.h>

/* Configure the line and assert it, before any driver comes up: PHI2 starts
 * inside that walk on machines where the RIA generates it. */
void resb_init(void);

/* Down, holding both parts in reset. Callable from any core and cheap enough
 * to sit inside a stop request -- a 6502 running beside the fan-out would
 * keep asking for what is being torn down. */
void resb_assert(void);

/* Up, after the run fan-out has brought the machine up for a program. The
 * 6502 fetches $FFFC next. */
void resb_release(void);

/* Whether a program is running or about to be -- not the pin. A machine with
 * a minimum hold time reports true for the whole window, while the line is
 * still low. */
bool resb_running(void);

#endif /* _CORE_WDC_RESB_H_ */
