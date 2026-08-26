/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* What the machine may ask of a console. Each machine writes its own driver --
 * a UART and CDC on the Pico, a ring the emulator fills, the APF bridge on a
 * Pocket -- and keeps its own pins, lifecycle and servers. */

#ifndef _CORE_COM_H_
#define _CORE_COM_H_

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

/* Guarded the way the pico-sdk guards it, so whichever header arrives first
 * wins and the other skips. */
#ifndef __printflike
#ifdef __GNUC__
#define __printflike(a, b) __attribute__((__format__(__printf__, a, b)))
#else
#define __printflike(a, b)
#endif
#endif

/* Where a byte came from, or which one to read. */
typedef enum
{
    COM_SOURCE_KEYBOARD,
    COM_SOURCE_UART,
    COM_SOURCE_TEL,
    COM_SOURCE_COUNT,
    COM_SOURCE_ANY = COM_SOURCE_COUNT,
} com_source_t;

// Non-blocking 1-byte read. *src is in/out:
//   - in COM_SOURCE_ANY: read from any active source via the sticky
//     RX picker. On byte, *src is set to the source that delivered;
//     on no byte, *src is reset to COM_SOURCE_ANY.
//   - in specific source: read only from that source. Bytes on other
//     sources are left in their FIFOs for a later reader. On no byte,
//     *src is reset to COM_SOURCE_ANY.
// Returns the byte (0..255), or negative when the requested source(s)
// have none. Which negative is the machine's own business.
int com_getchar(com_source_t *src);

// Non-blocking 1-byte peek at a specific source (UART/TEL), without
// consuming. Returns the byte (0..255), or negative when none is queued.
int com_peekchar(com_source_t src);

// Ensure putchar will not block even with a newline expansion
bool com_putchar_ready(void);

// Ensure space for com_write()
bool com_writable(void);

// Bypasses newline expansion. Caller must have checked com_writable() first.
void com_write(char ch);

// Console TX with newline (CRLF) expansion.
int com_putchar(int c);
__printflike(1, 2) int com_printf(const char *fmt, ...);

// The '\a' BEL alert
bool com_get_bel(void);
void com_set_bel(bool value);

/* A terminal query's answer (DSR/CPR/DA), entering the console's input as
 * though it had been typed -- as the UART source, ahead of typed input, since
 * the program asked for it and is waiting. Dropped rather than truncated if it
 * does not fit, and dropped entirely where a real terminal is attached and
 * will answer the host's query itself. */
void com_in_write_reply(const char *s, size_t n);

/* The console's merged input, as the OS's raw console read (TTY:) takes it:
 * up to count bytes, however many are queued now, 0 when none. Not the line
 * editor's door -- that one is com_getchar, and rln owns the editing.
 *
 * A machine whose 6502 reads the console through registers of its own stages
 * a byte there before the program asks, and this reclaims it before draining
 * the rings, oldest first. Mixing the two is undefined by the machine's own
 * documentation, which is what licenses the steal -- and without it that byte
 * is stranded until the program happens to read the register. */
size_t com_stdin_read(char *buf, size_t count);

/* The sink term.c hands over at init; the console fans printf output to it
 * alongside its own. */
void com_set_term_out(void (*out_chars)(const char *buf, int len));

#endif /* _CORE_COM_H_ */
