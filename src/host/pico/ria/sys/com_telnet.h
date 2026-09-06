/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The seam between the console and its telnet source -- com.c on one side,
 * com_telnet.c on the other, both halves of one module. Not to be confused with
 * net/telnet.h, which is the TCP layer this stands on.
 *
 * The telnet console, as the console sees it: a row for the picker to read,
 * peek and clear, the TX half the fan-out writes, and the pump its TCP stack
 * needs. What is configured about it -- the port, the key -- is com.h,
 * because that is what the monitor and the settings store reach for. A board
 * with no radio does not roster the row, and answers the TX half with
 * nothing.
 *
 * Private to sys/ and the RIA-W's roster. */

#ifndef _RIA_SYS_COM_TELNET_H_
#define _RIA_SYS_COM_TELNET_H_

#include "core/sys/com.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool com_telnet_tx_writable(void);
void com_telnet_tx_write(char ch);
size_t com_telnet_read(char *buf, size_t length);
int com_telnet_peek(void);

/* Service the TCP stack. Beware: this synchronously runs callbacks that can
 * disconnect the session and clear the rings, so a caller holding a ring
 * index must re-check after it returns. */
void com_telnet_pump(void);
void com_telnet_task(void);

/* Drop what is queued, and what a peer has already sent: the row's clear. */
void com_telnet_clear_rx(void);

#define COM_TELNET_SOURCE {.read = com_telnet_read, .peek = com_telnet_peek, .clear = com_telnet_clear_rx, .dwell_us = COM_WIRE_DWELL_US}

/* The other direction: what com.c lends its telnet half. */

/* Non-consuming peek at an SPSC RX ring (head==tail empty; the next byte
 * sits one past tail). The byte, or -1. */
int com_ring_peek(const uint8_t *buf, size_t size, size_t head, size_t tail);

#endif /* _RIA_SYS_COM_TELNET_H_ */
