/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_SYS_MEM_H_
#define _EMU_SYS_MEM_H_

#include "ria/sys/mem.h"

/* ------------------------------------------------------------------ */
/* Bus geometry (emulator-only)                                        */
/* ------------------------------------------------------------------ */

#define RIA_WINDOW_LO 0xFFE0
#define RIA_WINDOW_HI 0xFFF9 /* inclusive */

#define VIA_WINDOW_LO 0xFFD0
#define VIA_WINDOW_HI 0xFFDF /* inclusive */

extern uint8_t ram[0x10000];

#endif /* _EMU_SYS_MEM_H_ */
