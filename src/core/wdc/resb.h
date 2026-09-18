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

#endif /* _CORE_WDC_RESB_H_ */
