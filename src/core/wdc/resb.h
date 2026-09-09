/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * RESB, the 6502's reset line, which also resets the 6522 beside it.
 *
 * This is not a driver row. sys_stop asserts before it decides whether any
 * driver fan-out happens at all, so core/sys/sys.c brackets the machine with
 * these calls instead.
 */

#ifndef _CORE_WDC_RESB_H_
#define _CORE_WDC_RESB_H_

#include <stdbool.h>

void resb_init(void);

void resb_assert(void);

void resb_release(void);

/* Whether a program is running or about to be, which is not the state of the
 * pin: a machine with a minimum reset hold time reports true for the whole
 * window, while the line is still low. */
bool resb_running(void);

/* Put the line back where a savestate found it. This is not resb_assert,
 * because that also resets the 6502, the 6522, the parked bus and the running
 * clock rate, all of which a blob carries and their own rows have already
 * restored. Only a load may call this. */
void resb_restore(bool down);

#endif /* _CORE_WDC_RESB_H_ */
