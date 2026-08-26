/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _CORE_COM_COM_H_
#define _CORE_COM_COM_H_

#include "core/com.h"
#include <stddef.h>
#include <stdint.h>

/* The KEYBOARD source. hid/keyboard.c owns this ring and is the only caller: every host
 * keystroke, scripted or typed, enters the machine through one seam. */
void com_keyboard_push(const char *s, size_t n);
void com_keyboard_push_byte(uint8_t b);
size_t com_keyboard_free(void); /* ring headroom; the paste drip stays below it */

/* Cold-boot flush: clear both input rings and reset BEL (machine power-up). */
void com_init(void);

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

#endif /* _CORE_COM_COM_H_ */
