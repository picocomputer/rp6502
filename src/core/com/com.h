/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The console, as the rest of the machine sees it: the sources it reads, the
 * byte staged in the 6502's receive register, its terminal's answers, its
 * output, the bell, and starting a clean line. */

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

/* The terminals a console write is for, as a bitmap with one bit per source
 * that is a terminal, and COM_TERM_SCREEN for term.c, the terminal drawn on
 * the screen. The keyboard is not a terminal. term.c hears everything written
 * to the UART and answers on the UART's source; COM_TERM_SCREEN reaches it
 * alone, and nothing else that hears the UART. */
#define COM_TERM(src) (1u << (src))
#define COM_TERM_UART COM_TERM(COM_SOURCE_UART)
#define COM_TERM_TEL COM_TERM(COM_SOURCE_TEL)
#define COM_TERM_SCREEN COM_TERM(COM_SOURCE_COUNT)
#define COM_TERM_ALL (COM_TERM_UART | COM_TERM_TEL)

/* A source's row: read takes up to length bytes from the front of its
 * queue, peek returns the byte offset places from the front without taking
 * it, negative past the end, and size is how many bytes the queue holds. */
typedef struct
{
    size_t (*read)(char *buf, size_t length);
    int (*peek)(size_t offset);
    void (*clear)(void);
    uint32_t dwell_us;
    uint16_t size;
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

/* Up to n bytes from the named source, the byte staged from it first. */
size_t com_read(com_source_t src, char *buf, size_t n);

/* The byte offset places into the named source without taking it, negative
 * when there is none. Offset 0 is the staged byte when it came from that
 * source. */
int com_peek(com_source_t src, size_t offset);

/* How many bytes the named source's queue holds, the staged byte not
 * counted. Zero for a source the machine does not have. */
size_t com_queue_size(com_source_t src);

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

/* A terminal query's answer (DSR/CPR/DA) enters the UART's source as though
 * it had been typed, so whatever reads the console finds it in order with the
 * bytes around it. It waits behind the bytes the UART has queued, and once
 * its first byte is read the rest comes ahead of any that arrive after, so
 * it never lands inside a sequence; an answer stored then joins it. One that
 * does not fit is dropped. */
void com_reply(const char *s, size_t n);

/* While quiet, the console's terminal does not answer a query: attribute
 * 0x0D. A program starts, and the monitor goes on after it, with the
 * machine's default, which is quiet where another terminal is the screen the
 * user sees and answers itself: the RIA with no VGA, and an emulator printing
 * to a host terminal. com_set_reply_default sets the quiet flag too. */
bool com_get_reply_quiet(void);
void com_set_reply_quiet(bool quiet);
void com_set_reply_default(bool quiet);
void com_run(void);

__printflike(1, 2) int com_printf(const char *fmt, ...);

/* com_printf for a UTF-8 format, its output converted to the code page. */
__printflike(1, 2) int com_printf_utf8(const char *utf8_fmt, ...);

/* Whether a BEL the console sends rings the bell. */
bool com_get_bel(void);
void com_set_bel(bool value);

/* Flushes, then CR LF unless the console's terminal has its cursor in
 * column 1. */
void com_clean_line(void);

/* The end of a session: the soft reset and the default cursor shape, then a
 * clean line. */
void com_end_reset(void);

/* The picker's held source (1) and its deadline (8), the bell (1), the
 * staged byte's source (1), and the held answer's length (1), how much of it
 * has been read (1), its buffer (32) and the quiet flag (1). */
#define COM_SST_SIZE 46
void com_sst_save(sst_cursor_t *c, unsigned flags);
bool com_sst_load(sst_cursor_t *c, unsigned flags);

#define COM_DRIVER DRIVER(       \
    INIT(com_rx_clear)           \
    RUN(com_run)                 \
    STOP(com_run)                \
    BREAK(com_rx_clear)          \
    SST(COM_, 6, COM_SST_SIZE, com_sst_save, com_sst_load))

#endif /* _CORE_COM_COM_H_ */
