/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The console input multiplexer (com.c). The firmware multiplexes several byte
 * streams (keyboard, UART, telnet); the emulator needs only two for the line
 * editor (rln.c):
 *   - COM_SOURCE_KBD  the user's typing, injected by the kbd.c replacement.
 *   - COM_SOURCE_UART the terminal's in-band replies (CPR/DA) that term.c emits
 *                     via com_in_write_reply when rln probes geometry.
 * com.c implements the rings; the signatures + COM_SOURCE_* ordering match the
 * firmware so the vendored rln.c/term.c (which reach this via the firmware path
 * "sys/com.h" — the shim there forwards to this header) compile unchanged.
 */

#ifndef _EMU_COM_H_
#define _EMU_COM_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Must match ria/sys/com.h: rln treats UART/TEL as protocol-tracked sources
 * (they answer CPR/DA), KBD as untracked typed input. */
typedef enum
{
    COM_SOURCE_KBD,
    COM_SOURCE_UART,
    COM_SOURCE_TEL,
    COM_SOURCE_COUNT,
    COM_SOURCE_ANY = COM_SOURCE_COUNT,
} com_source_t;

/* Read one byte. With *src == COM_SOURCE_ANY, picks any ready source and sets
 * *src to it; otherwise reads only *src. Returns -1 (and *src = ANY) when no
 * byte is available. Mirrors the firmware contract rln_read_next relies on. */
int com_getchar(com_source_t *src);

/* Peek the next byte of a specific source without consuming it, or -1. */
int com_peekchar(com_source_t src);

/* term.c routes terminal query replies (DSR/CPR/DA) here; they enter rln's
 * input as the UART (protocol-tracked) source. */
#define COM_IN_BUF_SIZE 16
void com_in_write_reply(const char *s, size_t n);

/* The kbd.c replacement injects user keystrokes as the KBD source. */
void com_kbd_push(const char *s, size_t n);
void com_kbd_push_byte(uint8_t b);

/* Clear both rings (machine reset). */
void com_reset(void);

/* Bell-enable flag, exposed through the BEL attribute (vendored atr.c). The
 * emulator stores it but has no teletype bell. */
bool com_get_bel(void);
void com_set_bel(bool value);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_COM_H_ */
