/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* What the machine may ask of a console. Each machine writes its own driver --
 * a UART and CDC on the Pico, a ring the emulator fills, the APF bridge on a
 * Pocket -- and keeps its own pins, bring-up and servers. */

#ifndef _CORE_SYS_COM_H_
#define _CORE_SYS_COM_H_

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Guarded the way the pico-sdk guards it, so whichever header arrives first
 * wins and the other skips. */
#ifndef __printflike
#ifdef __GNUC__
#define __printflike(a, b) __attribute__((__format__(__printf__, a, b)))
#else
#define __printflike(a, b)
#endif
#endif

/* Where a byte came from, or which one to read. The order is the order the
 * picker tries them: keyboard, then wire, then remote. */
typedef enum
{
    COM_SOURCE_KEYBOARD,
    COM_SOURCE_UART,
    COM_SOURCE_TEL,
    COM_SOURCE_COUNT,
    COM_SOURCE_ANY = COM_SOURCE_COUNT,
} com_source_t;

/* One console input source, as the picker reads it. A machine builds its
 * table from the RP6502_COM_SOURCES rows its drivers.h lists, indexed by
 * com_source_t; a source it has not got is a row it does not name, and
 * reads nothing. */
typedef struct
{
    size_t (*read)(char *buf, size_t length);
    int (*peek)(void);   /* next byte or -1; NULL where a reader may not look */
    void (*clear)(void); /* drop what is queued: a cold boot and a break */
    uint32_t dwell_us;   /* how long a dry source keeps the reader */
} com_source_driver_t;

/* A source fed in bursts -- a wire, or a ring a paste drips into a chunk at
 * a time -- is empty between two chunks, so a dry read keeps the reader for
 * this long. A source whose empty queue means nobody is typing sets 0. One
 * number for every burst source, because they are the same case. */
#define COM_WIRE_DWELL_US 1000

/* Reset the picker and clear every listed row: a cold boot and a break. */
void com_rx_clear(void);

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

// Non-blocking 1-byte peek at a specific source, without consuming. Returns
// the byte (0..255), or negative when none is queued -- which is also the
// answer for a source this machine keeps no queue for, since the reader
// cannot tell "nothing yet" from "never" and must not have to.
int com_peekchar(com_source_t src);

// A machine whose 6502 reads the console through registers commits a byte
// there ahead of any reader, and a reader some other way has to be able to
// get it back. Both answer for one source only, the one whose row is about
// to be read: the byte it staged, taken (zero length leaves it staged) or
// looked at (-1 when none). A machine whose bus is fabric stages nothing.
size_t com_rx_reclaim(char *buf, size_t length, com_source_t src);
int com_rx_peek(com_source_t src);

// Ensure space for com_write()
bool com_writable(void);

// Bypasses newline expansion. Caller must have checked com_writable() first.
void com_write(char ch);

// Console TX with newline (CRLF) expansion.
int com_putchar(int c);
__printflike(1, 2) int com_printf(const char *fmt, ...);

/* The program's two output streams, as many bytes as the console can take
 * now, which is the count answered. stdout is the console with newline
 * expansion. stderr is a stream of its own: the terminal shows it beside
 * stdout, and a machine with a stderr of its own gets it there as well. */
size_t com_stdout_write(const char *buf, size_t count);
size_t com_stderr_write(const char *buf, size_t count);

// The '\a' BEL alert
bool com_get_bel(void);
void com_set_bel(bool value);

/* The console's merged input, as the OS's raw console read (TTY:) takes it:
 * up to count bytes, however many are queued now, 0 when none. Not the line
 * editor's door -- that one is com_getchar, and rln owns the editing. */
size_t com_stdin_read(char *buf, size_t count);

#endif /* _CORE_SYS_COM_H_ */
