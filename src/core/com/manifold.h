/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The contract a machine fulfils for the console: where com's output goes,
 * the UART source com reads, the terminal emulator's link, and the software
 * console of a machine with no serial port. A machine implements the parts it
 * uses, in a file called manifold.c. */

#ifndef _CORE_COM_MANIFOLD_H_
#define _CORE_COM_MANIFOLD_H_

#include "core/com/com.h"
#include "core/sys/sst.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Console: com's output calls these, and the UART source is a row of
 * RP6502_COM_SOURCES. */

/* Room for one character, a newline's CR LF included. */
bool manifold_cts(void);

/* One character, a newline going out as CR LF. */
int manifold_putchar(int c);

/* One byte as it is. */
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

/* A terminal query's answer (DSR/CPR/DA) enters the console's input as though
 * it had been typed, so whatever reads the console finds it in order with the
 * bytes around it. It is held whole until that input is empty and dropped
 * rather than truncated when it does not fit, so it can never land inside a
 * sequence still being delivered. A machine with a real terminal on the far
 * end, which answers the query itself, drops the reply instead. */
void manifold_reply(const char *s, size_t n);

/* While suppress is set, the console's terminal does not answer a query:
 * attribute 0x0D. Each program starts with the machine's default, which is
 * on where another terminal is the screen the user sees and answers itself:
 * the RIA with no VGA, and an emulator printing to a host terminal. */
void manifold_suppress_reply(bool suppress);
bool manifold_replies_suppressed(void);

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

/* The default of manifold_suppress_reply that each program starts with. */
void manifold_set_reply_default(bool suppress);

/* The tx tap is given every byte the console sends to the UART. The std tap
 * is given a program's output with its fd, before the newlines are expanded.
 * The stderr sink is given a program's stderr. */
void manifold_set_tx_tap(void (*tap)(const char *buf, int len));
void manifold_set_std_tap(void (*tap)(int fd, const char *buf, int len));
void manifold_set_stderr_sink(void (*sink)(const char *buf, int len));

void manifold_task(void);
void manifold_run(void);

/* The 35 fixed bytes are the reply length (1), the reply buffer (32), the
 * CR LF latch (1) and the replies' suppression (1). Each of the two rings
 * adds a 16-bit count and MANIFOLD_RING_SIZE bytes. */
#define MANIFOLD_SST_SIZE (2 * (2 + MANIFOLD_RING_SIZE) + 35)
void manifold_sst_save(sst_cursor_t *c, unsigned flags);
bool manifold_sst_load(sst_cursor_t *c, unsigned flags);

#define MANIFOLD_DRIVER DRIVER(     \
    TASK_PUMPED(manifold_task)      \
    RUN(manifold_run)               \
    STOP(manifold_run)              \
    SST(MFLD, 2, MANIFOLD_SST_SIZE, manifold_sst_save, manifold_sst_load))

#endif /* _CORE_COM_MANIFOLD_H_ */
