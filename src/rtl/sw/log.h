/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_LOG_H_
#define _FPGA_SW_LOG_H_

/* Diagnostic narration. It exists to fill the log, so it goes silent
 * with it: an off build must not spend console bytes, because every four
 * of them is a target command on the bridge the drive and the staging
 * store share. Callers include com.h. */
#ifdef RP6502_LOG_FILE
#define LOG_SAY(...) com_printf(__VA_ARGS__)
#else
#define LOG_SAY(...) ((void)0)
#endif

#ifdef RP6502_LOG_FILE

void log_init(void);
void log_task(void);
/* Every console byte, from com_tx_write's fan-out. */
void log_putc(char c);

#else

/* The off build has no ring, no descriptor and no task. The switch is
 * here rather than at each call site so com.c and main.c read the same
 * either way. */
static inline void log_init(void) {}
static inline void log_task(void) {}
static inline void log_putc(char c) { (void)c; }

#endif

#endif /* _FPGA_SW_LOG_H_ */
