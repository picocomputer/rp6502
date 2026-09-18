/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

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

/* On the RIA-W, com_telnet_pump can run the accept and disconnect callbacks
 * before it returns. When tcp_write fails with an error other than ERR_MEM
 * during com_telnet_drain_tx, the connection is closed, which calls the
 * disconnect callback. When the TX ring is still full after the drain,
 * com_telnet_pump runs cyw_task, which can call either callback. Those
 * callbacks can start or end a session and clear the rings, so a caller
 * holding a ring index must check it again after com_telnet_pump returns. */
void com_telnet_pump(void);
void com_telnet_task(void);

void com_telnet_clear_rx(void);

#define COM_TELNET_SOURCE {.read = com_telnet_read, .peek = com_telnet_peek, .clear = com_telnet_clear_rx, .dwell_us = COM_WIRE_DWELL_US}

int com_ring_peek(const uint8_t *buf, size_t size, size_t head, size_t tail);

#endif /* _RIA_SYS_COM_TELNET_H_ */
