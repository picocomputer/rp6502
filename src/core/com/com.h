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
#include <stdint.h>

/* The KEYBOARD source: every host keystroke, scripted or typed, enters the
 * machine here. The pushers are core/hid/vtkeys.c and the Pocket's own com.c.
 * A RIA is not among them -- it links no core/com at all, and its console
 * pulls from keymap_in_chars instead, which is why the keymap keeps a queue. */
void com_keyboard_push(const char *s, size_t n);
void com_keyboard_push_byte(uint8_t b);
#define COM_RING_SIZE 64 /* each ring; a power of two */
size_t com_keyboard_free(void); /* ring headroom; the paste drip stays below it */

/* The UART source: a machine whose console has a wire pushes what arrived on
 * it here, the way a Pico drains its UART FIFO. A Ctrl-C latches SIGINT
 * before the space check, so a break is caught even when the ring is full and
 * the byte is dropped. */
void com_uart_push(const char *s, size_t n);
size_t com_uart_free(void); /* headroom; a wire reads no more than this */

/* Nothing queued anywhere, from any source, with nothing held back: what a
 * host asks before it decides its input has genuinely run out. */
bool com_input_idle(void);

/* Stop answering the terminal queries a program sends, because something at
 * the far end of the wire is a real terminal and will answer them itself.
 * The Pico's VGA chip has the same switch, thrown by a live CDC or telnet
 * connection. Wiring, not machine state: com_init does not touch it. */
void com_suppress_term_reply(bool suppress);

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

/* Drain the wire both ways. The host that is linked defines it -- a machine
 * whose console is a UART has real work here, one whose console is the
 * terminal the walk already reaches has nothing to do here. */
void com_task(void);

/* This driver's row in a machine's driver list; see core/sys/driver.h. */
#define COM_DRIVER DRIVER(com_init, com_task, nul_task, com_run, com_stop, com_break, nul_config, nul_config)

#endif /* _CORE_COM_COM_H_ */
