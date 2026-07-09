/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_SYS_COM_H_
#define _EMU_SYS_COM_H_

#include "ria/sys/com.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* term.c routes terminal query replies (DSR/CPR/DA) here; they enter rln's
 * input as the UART (protocol-tracked) source. */
#define COM_IN_BUF_SIZE 16
void com_in_write_reply(const char *s, size_t n);

/* The kbd.c replacement injects user keystrokes as the KBD source. */
void com_kbd_push(const char *s, size_t n);
void com_kbd_push_byte(uint8_t b);

/* Cold-boot flush: clear both input rings and reset BEL (machine reset). The
 * per-program-start BEL reset that keeps type-ahead is com_set_bel in std_reset. */
void com_reset(void);

/* The wire to the terminal: the pico stdio shim hands the captured driver's
 * out_chars here (the analog of the firmware's UART/PIX fanout target). */
void com_set_term_out(void (*out_chars)(const char *buf, int len));

/* The single terminal sink (the firmware UART-drain analog): tap/echo/BEL
 * observe every terminal-bound byte here, then it goes out the wire. */
void com_tx_write(const char *buf, int len);

/* Tap the terminal OUT stream (NULL to clear). Used by tests to assert
 * program output without rendering a frame. */
void com_set_tx_tap(void (*tap)(const char *buf, int len));

#ifdef __cplusplus
}
#endif

#endif /* _EMU_SYS_COM_H_ */
