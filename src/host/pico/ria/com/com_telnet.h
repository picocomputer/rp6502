/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The console's telnet server on the RIA-W. A session that has given the
 * passkey is a source of the console and gets a copy of its output. The RIA
 * has no server, and output sent to it goes nowhere. */

#ifndef _RIA_COM_COM_TELNET_H_
#define _RIA_COM_COM_TELNET_H_

#include "core/com/com.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A session has given the passkey and is logged in. Never on the RIA. */
bool com_telnet_connected(void);

bool com_telnet_tx_writable(void);
void com_telnet_tx_write(char ch);
size_t com_telnet_read(char *buf, size_t length);
int com_telnet_peek(size_t offset);

/* On the RIA-W, com_telnet_pump can run the accept and disconnect callbacks
 * before it returns. It first sends what it can of the TX ring, and when
 * tcp_write fails with an error other than ERR_MEM the connection is closed,
 * which calls the disconnect callback. When the TX ring is still full after
 * that, com_telnet_pump runs cyw_task, which can call either callback. Those
 * callbacks can start or end a session and clear the rings, so a caller
 * holding a ring index must check it again after com_telnet_pump returns. */
void com_telnet_pump(void);
void com_telnet_task(void);
void com_telnet_flashing(void);

void com_telnet_clear_rx(void);

#define COM_TELNET_RX_SIZE 32
#define COM_TELNET_SOURCE {.read = com_telnet_read, .peek = com_telnet_peek, \
    .clear = com_telnet_clear_rx, .dwell_us = COM_DWELL_US, .size = COM_TELNET_RX_SIZE}

// Telnet console server settings
#define COM_TELNET_KEY_SIZE 33
int com_telnet_port_response(char *buf, size_t buf_size, int state, unsigned width);
int com_telnet_key_response(char *buf, size_t buf_size, int state, unsigned width);

#define COM_TELNET_CONFIG_PORT CONFIG_INT(O, com_telnet, port, uint16_t, 23, \
    nul_check, nul_apply, STR_PORT, com_telnet_port_response, STR_HELP_SET_PORT, NULL)
#define COM_TELNET_CONFIG_KEY CONFIG_STR(A, com_telnet, key, COM_TELNET_KEY_SIZE, "", \
    nul_check, nul_apply, STR_KEY, com_telnet_key_response, STR_HELP_SET_KEY, NULL)
#define COM_TELNET_DRIVER DRIVER( \
    TASK_ALWAYS(com_telnet_task)  \
    FLASHING(com_telnet_flashing) \
    COM_TELNET_CONFIG_PORT        \
    COM_TELNET_CONFIG_KEY)

#endif /* _RIA_COM_COM_TELNET_H_ */
