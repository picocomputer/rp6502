/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The contract a machine fulfils for the console: where its output goes, the
 * UART source com reads, the terminal emulator's link, and the software
 * console of a machine with no serial port. A machine implements the parts it
 * uses, in a file called manifold.c. */

#ifndef _CORE_COM_MANIFOLD_H_
#define _CORE_COM_MANIFOLD_H_

#include "core/com/com.h"
#include "core/sys/sst.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Console: its output, and the UART source, a row of RP6502_COM_SOURCES. */

/* Clear to send: room for one character, a newline's CR LF included, so the
 * manifold_putchar or manifold_putchar_raw after it does not block. */
bool manifold_cts(void);

/* One character, a bare '\n' going out as CR LF. The last byte written is
 * remembered across calls, so a '\r' sent in an earlier call still keeps the
 * '\n' after it from being expanded. */
int manifold_putchar(int c);

/* One byte as it is, without checking for room. */
void manifold_putchar_raw(char c);

/* A program's stdout or stderr, fd STDOUT_FILENO or STDERR_FILENO, its
 * newlines going out as CR LF. Writes what fits and returns the count. */
size_t manifold_write(int fd, const char *buf, size_t n);

/* Bytes as they are, to the terminals in the COM_TERM_ bitmap to. They are
 * queued behind everything written before them and ahead of everything
 * written after, waiting for room as manifold_putchar does. */
void manifold_write_to(uint8_t to, const char *buf, size_t n);

/* Returns once every byte queued so far has gone out. */
void manifold_flush(void);

/* The UART's queue holds MANIFOLD_RING_SIZE bytes, which machine.h sets. */
size_t manifold_uart_read(char *buf, size_t length);
int manifold_uart_peek(size_t offset);
void manifold_uart_clear(void);
#define MANIFOLD_UART_SOURCE {.read = manifold_uart_read, .peek = manifold_uart_peek, \
    .clear = manifold_uart_clear, .dwell_us = COM_DWELL_US, .size = MANIFOLD_RING_SIZE}

/* Terminal: the terminal emulator's link to the console. */

/* The answer of the terminal in core/term/cells.c to a query: com_reply where
 * that terminal is the console's, dropped where another chip answers. */
void manifold_reply(const char *s, size_t n);

/* The terminal emulator's input: every byte the console sends to the UART or
 * to COM_TERM_SCREEN. */
void manifold_set_term_out(void (*out_chars)(const char *buf, int len));

/* Software console: the keyboard and UART rings of a machine with no serial
 * port, the host terminal that feeds the UART ring, and the taps that see the
 * console's output. */

/* A Ctrl-C in what is pushed raises the SIGINT, even when the ring is full and
 * the bytes are dropped. */
void manifold_keyboard_push(const char *s, size_t n);
void manifold_keyboard_push_byte(uint8_t b);
void manifold_uart_push(const char *s, size_t n);

size_t manifold_keyboard_free(void);

size_t manifold_keyboard_read(char *buf, size_t length);
int manifold_keyboard_peek(size_t offset);
void manifold_keyboard_clear(void);

/* The host terminal: tx is given every byte the console sends to the UART, and
 * manifold_task reads rx into the UART ring. A console wire is a person
 * typing, where a Ctrl-C raises the SIGINT; a stream wire is a pipe or a file,
 * where 0x03 is an ordinary byte. Either function may be NULL. */
void manifold_set_wire(void (*tx)(const char *buf, int len),
                       size_t (*rx)(char *buf, size_t max), bool stream);

/* The tx tap is given every byte the console sends to the UART. The std tap
 * is given a program's output with its fd, before the newlines are expanded.
 * The stderr sink is given a program's stderr. */
void manifold_set_tx_tap(void (*tap)(const char *buf, int len));
void manifold_set_std_tap(void (*tap)(int fd, const char *buf, int len));
void manifold_set_stderr_sink(void (*sink)(const char *buf, int len));

void manifold_task(void);

/* The two rings, a 16-bit count and MANIFOLD_RING_SIZE bytes each, and the
 * CR LF latch (1). */
#define MANIFOLD_SST_SIZE (2 * (2 + MANIFOLD_RING_SIZE) + 1)
void manifold_sst_save(sst_cursor_t *c, unsigned flags);
bool manifold_sst_load(sst_cursor_t *c, unsigned flags);

#define MANIFOLD_DRIVER DRIVER(     \
    TASK_PUMPED(manifold_task)      \
    SST(MFLD, 3, MANIFOLD_SST_SIZE, manifold_sst_save, manifold_sst_load))

#endif /* _CORE_COM_MANIFOLD_H_ */
