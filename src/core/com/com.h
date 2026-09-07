/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _CORE_COM_COM_H_
#define _CORE_COM_COM_H_

#include "core/sys/com.h"
#include <stddef.h>
#include "core/sys/sst.h"
#include <stdint.h>

/* The KEYBOARD source, for a machine whose host resolved the keystroke into
 * text before it arrived: core/hid/vtkeys.c pushes here. A machine with a
 * layout engine of its own lists keymap's queue as its keyboard row instead,
 * and neither the Pocket nor the RIA pushes here at all. */
void com_keyboard_push(const char *s, size_t n);
void com_keyboard_push_byte(uint8_t b);
size_t com_keyboard_free(void); /* ring headroom; the paste drip stays below it */

/* The row core/com/pick.c reads it through. A dwell, because the paste drip
 * fills this ring a chunk at a time and the line editor drains it dry every
 * pass: a dry ring mid-paste is a gap in the drip, not a person who stopped. */
size_t com_keyboard_read(char *buf, size_t length);
void com_keyboard_clear(void);
#define COM_KEYBOARD_SOURCE {.read = com_keyboard_read, .clear = com_keyboard_clear, .dwell_us = COM_WIRE_DWELL_US}

/* The UART source: a machine whose console has a wire pushes what arrived on
 * it here, the way a Pico drains its UART FIFO. A Ctrl-C latches SIGINT
 * before the space check, so a break is caught even when the ring is full and
 * the byte is dropped. A stream -- a pipe or a file -- comes in the second
 * door onto the same ring: a 0x03 on it is a byte, and its caller pushes no
 * more than fits, so nothing on it is ever dropped. */
void com_uart_push(const char *s, size_t n);
void com_stream_push(const char *s, size_t n);
size_t com_uart_free(void); /* headroom; a wire reads no more than this while there is some */

/* The row core/com/pick.c reads it through. The emulated terminal's answer
 * to a query is this row's tail: promoted into the ring only once the wire
 * is empty, so it never lands inside something the wire had half-delivered.
 * Every software machine lists this row, the Pocket included, because that
 * is where its terminal's answers arrive. */
size_t com_uart_read(char *buf, size_t length);
int com_uart_peek(void);
void com_uart_clear(void);
#define COM_UART_SOURCE {.read = com_uart_read, .peek = com_uart_peek, .clear = com_uart_clear, .dwell_us = COM_WIRE_DWELL_US}

/* Nothing queued anywhere, from any source, with nothing held back: what a
 * host asks before it decides its input has genuinely run out. */
bool com_input_idle(void);

/* Cold-boot flush: clear the input and reset BEL (machine power-up). */
void com_init(void);

/* A break drops what was typed at a machine that is being interrupted: the
 * type-ahead was meant for the program being stopped, not for whatever comes
 * next. The Pico's console has always done this; this one had no hook at all.
 */
void com_break(void);

/* The machine is going away and the screen it drew is somebody's terminal:
 * hand it back the way it was found rather than wearing what the guest set.
 */
void com_stop(void);

/* Program start: restore the BEL default, keeping queued input (type-ahead
 * survives an exec). The cold-boot ring flush is com_init. */
void com_run(void);

/* The single terminal sink: the tap, the bell and the wire all observe every
 * terminal-bound byte here, once, after CRLF translation. */
void com_tx_write(const char *buf, int len);

/* CRLF-translate, then the sink. Where a machine's own com_printf ends. */
void com_crlf_write(const char *buf, int len);

/* Tap the terminal OUT stream (NULL to clear). Used by tests to assert
 * program output without rendering a frame. */
void com_set_tx_tap(void (*tap)(const char *buf, int len));

/* Tap the program's own streams before any translation (NULL to clear): fd 1
 * for everything it sends to stdout, cooked or raw, fd 2 for stderr. One
 * consumer at a time: the desktop's mirror, the debug adapter, a test. */
void com_set_std_tap(void (*tap)(int fd, const char *buf, int len));

/* Where the program's stderr goes besides the terminal (NULL to clear). The
 * terminal shows it either way; this is the second copy, for a host with a
 * stderr of its own to put it on. A host that installs nothing sends the
 * program's errors nowhere but the screen, which is what a machine running
 * inside someone else's process must do. */
void com_set_stderr_sink(void (*sink)(const char *buf, int len));

/* Drain the wire both ways. The host that is linked defines it -- a machine
 * whose console is a UART has real work here, one whose console is the
 * terminal the walk already reaches has nothing to do here. */
void com_task(void);

/* This driver's row in a machine's driver list; see core/sys/driver.h. */
/* Both input rings, the terminal's held answer, the bell setting, the CRLF
 * latch, and pick's source hold. The wiring is not here: the taps, the wire
 * and the reply suppression are the host's, installed once before any load
 * can happen, and libretro installs no wire at all.
 * 2 * (COM_RING_SIZE + 4) + 1 + 32 + 1 + 1 + 9 */
#define COM_SST_SIZE (2 * (COM_RING_SIZE + 4) + 44)
void com_sst_save(sst_cursor_t *c, unsigned flags);
bool com_sst_load(sst_cursor_t *c, unsigned flags);

#define COM_DRIVER DRIVER(com_init, com_task, nul_task, com_run, com_stop, com_break, \
    nul_config, nul_config, SST(COM_, 1, COM_SST_SIZE, com_sst_save, com_sst_load))

#endif /* _CORE_COM_COM_H_ */
