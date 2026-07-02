/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The console multiplexer (com.c). Implements the shared contract from
 * ria/sys/com.h — the firmware multiplexes keyboard/UART/telnet; the emulator
 * needs only two streams for the line editor (rln.c):
 *   - COM_SOURCE_KBD  the user's typing, injected by the kbd.c replacement.
 *   - COM_SOURCE_UART the terminal's in-band replies (CPR/DA) that term.c emits
 *                     via com_in_write_reply when rln probes geometry.
 * The rest of this header is the emulator's injection API. Vendored firmware
 * sources reach both via the firmware path "sys/com.h" — the shim there
 * forwards to this header.
 */

#ifndef _EMU_COM_H_
#define _EMU_COM_H_

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

/* Clear both rings (machine reset). */
void com_reset(void);

/* The wire to the terminal: the pico stdio shim hands the captured driver's
 * out_chars here (the analog of the firmware's UART/PIX fanout target). */
void com_set_term_out(void (*out_chars)(const char *buf, int len));

/* The single terminal sink (the firmware UART-drain analog): tap/echo/BEL
 * observe every terminal-bound byte here, then it goes out the wire. */
void com_out_chars(const char *buf, int len);

/* Tap the terminal OUT stream (NULL to clear). Used by tests to assert
 * program output without rendering a frame. */
void com_set_out_tap(void (*tap)(const char *buf, int len));

#ifdef __cplusplus
}
#endif

#endif /* _EMU_COM_H_ */
