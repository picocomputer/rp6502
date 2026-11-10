/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The console, as the rest of the machine sees it: the sources it reads, the
 * byte staged in the 6502's receive register, its output, the bell, and
 * whether the cursor is at the start of a clean line. */

#ifndef _CORE_COM_COM_H_
#define _CORE_COM_COM_H_

#include <stdarg.h>
#include "core/sys/sst.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef __printflike
#ifdef __GNUC__
#define __printflike(a, b) __attribute__((__format__(__printf__, a, b)))
#else
#define __printflike(a, b)
#endif
#endif

/* The picker tries the sources in the order they are declared here, keyboard
 * first. */
typedef enum
{
    COM_SOURCE_KEYBOARD,
    COM_SOURCE_UART,
    COM_SOURCE_TEL,
    COM_SOURCE_COUNT,
    COM_SOURCE_ANY = COM_SOURCE_COUNT,
} com_source_t;

typedef struct
{
    size_t (*read)(char *buf, size_t length);
    int (*peek)(void);
    void (*clear)(void);
    uint32_t dwell_us;
} com_source_driver_t;

/* A source fed in bursts is empty between two chunks, so after a byte arrives
 * that source keeps the reader for this long before the picker will try
 * another one. */
#define COM_DWELL_US 1000

/* After this long, a full ring is read into and the bytes dropped, so that a
 * Ctrl-C typed behind the type-ahead is still seen. The rest of what was
 * typed is lost. */
#define COM_HOLD_MS 5000

void com_rx_clear(void);
void com_rx_clear_source(com_source_t src);

/* Non-blocking 1-byte read, negative when there is none. *src is in and out:
 * COM_SOURCE_ANY reads whatever the picker chooses and names it on the way
 * out, a named source reads only that one and leaves the others queued. */
int com_getchar(com_source_t *src);

/* Negative when there is none. COM_SOURCE_ANY peeks every source. */
int com_peekchar(com_source_t src);

size_t com_stdin_read(char *buf, size_t count);

/* com_rx_stage puts the picker's next byte in the 6502's receive register,
 * false when there is none. The byte waits there ahead of any reader, so a
 * reader coming at the console another way has to take that byte back before
 * the source's own row is read; otherwise the two arrive out of order.
 * com_rx_reclaim and com_rx_peek answer only for the source the byte came
 * from. A length of zero leaves the byte staged. */
bool com_rx_stage(void);
size_t com_rx_reclaim(char *buf, size_t length, com_source_t src);
int com_rx_peek(com_source_t src);

/* Clear to send: there is room for one character, a newline's CR LF
 * included, so the com_putchar or com_putchar_raw after it does not block. */
bool com_cts(void);

/* Console TX, a bare '\n' expanded to CRLF. The '\r' that suppresses the
 * expansion may have come from an earlier call, because the last byte written
 * is remembered across them; a buffer is not expanded independently of the one
 * before it. */
int com_putchar(int c);

/* Bypasses newline expansion and does not check for room. */
void com_putchar_raw(char c);

/* A program's stdout or stderr, fd STDOUT_FILENO or STDERR_FILENO, with its
 * newlines expanded as com_putchar expands them. Writes what fits and returns
 * the count. */
size_t com_write(int fd, const char *buf, size_t n);

__printflike(1, 2) int com_printf(const char *fmt, ...);

/* com_printf for a UTF-8 format, its output converted to the code page. */
__printflike(1, 2) int com_printf_utf8(const char *utf8_fmt, ...);

/* Returns once every console byte queued so far has gone out. */
void com_flush(void);

/* Whether a BEL the console sends rings the bell. */
bool com_get_bel(void);
void com_set_bel(bool value);

/* Every console byte passes here on its way out. */
void com_line_track(const char *buf, size_t len);

/* True when a CR and an LF have gone out since the last other byte. */
bool com_line_clean(void);

/* CR LF, unless the line is already clean. */
void com_clean_line(void);

/* The caller has left the cursor at the start of a clean line. */
void com_line_set_clean(void);

/* The end of a session: the soft reset, then CR LF unless the line was clean
 * before it. */
void com_end_reset(void);

/* The picker's held source (1) and its deadline (8), the bell (1), the
 * staged byte's source (1), and the clean-line state (1). */
#define COM_SST_SIZE 12
void com_sst_save(sst_cursor_t *c, unsigned flags);
bool com_sst_load(sst_cursor_t *c, unsigned flags);

#define COM_DRIVER DRIVER(       \
    INIT(com_rx_clear)           \
    BREAK(com_rx_clear)          \
    SST(COM_, 4, COM_SST_SIZE, com_sst_save, com_sst_load))

#endif /* _CORE_COM_COM_H_ */
