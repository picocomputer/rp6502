/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_SYS_COM_H_
#define _CORE_SYS_COM_H_

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
#define COM_WIRE_DWELL_US 1000

/* After this long, a full ring is read into and the bytes dropped, so that a
 * Ctrl-C typed behind the type-ahead is still seen. The rest of what was
 * typed is lost. */
#define COM_WIRE_HOLD_MS 5000

void com_rx_clear(void);

void com_rx_save(sst_cursor_t *c);
bool com_rx_load(sst_cursor_t *c);

/* Non-blocking 1-byte read, negative when there is none. *src is in and out:
 * COM_SOURCE_ANY reads whatever the picker chooses and names it on the way
 * out, a named source reads only that one and leaves the others queued. */
int com_getchar(com_source_t *src);

int com_peekchar(com_source_t src);

/* A machine whose 6502 reads the console through registers stages a byte there
 * ahead of any reader, so a reader coming at the console another way has to
 * take that byte back before the source's own row is read; otherwise the two
 * arrive out of order. A length of zero leaves the byte staged. */
size_t com_rx_reclaim(char *buf, size_t length, com_source_t src);
int com_rx_peek(com_source_t src);

bool com_writable(void);

/* Bypasses newline expansion and does not check for room. */
void com_write(char ch);

/* Console TX, a bare '\n' expanded to CRLF. The '\r' that suppresses the
 * expansion may have come from an earlier call, because the last byte written
 * is remembered across them; a buffer is not expanded independently of the one
 * before it. */
int com_putchar(int c);
__printflike(1, 2) int com_printf(const char *fmt, ...);

size_t com_stdout_write(const char *buf, size_t count);
size_t com_stderr_write(const char *buf, size_t count);

bool com_get_bel(void);
void com_set_bel(bool value);

size_t com_stdin_read(char *buf, size_t count);

#endif /* _CORE_SYS_COM_H_ */
