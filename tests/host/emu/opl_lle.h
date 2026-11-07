/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* YM3812-LLE, nukeykt's gate-level model of the die, driven only through its
 * pins: the oracle for the engine. */

#ifndef _TESTS_HOST_EMU_OPL_LLE_H_
#define _TESTS_HOST_EMU_OPL_LLE_H_

#include "opl_script.h"

/* Runs the die for schedule->samples samples, each write put on the bus so
 * that it takes effect as if applied just before its sample, and fills out
 * with the YM3014B word of each sample. IC is held ic_frames samples before
 * sample 0 starts. Returns NULL, or why a write cannot go on the bus there. */
const char *lle_run(const opl_script_t *schedule, unsigned ic_frames, int16_t *out);

/* Whether the bus can take w after prev: its address write waits 19 cycles
 * after an operator or channel write strobe and 4 after a global one. */
bool lle_takes(const opl_write_t *prev, const opl_write_t *w);

#endif /* _TESTS_HOST_EMU_OPL_LLE_H_ */
