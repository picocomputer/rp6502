/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_COM_H_
#define _RIA_SYS_COM_H_

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "core/sys/com.h"

#define COM_UART uart1
#define COM_UART_BAUD_RATE 115200
#define COM_UART_TX_PIN 4
#define COM_UART_RX_PIN 5

/* Main events
 */

void com_init(void);
void com_run(void);
void com_task(void);
void com_stop(void);
void com_break(void);

/* com_stdio_driver sets crlf_enabled, so a newline can be written to the ring
 * as CR LF. com_putchar_ready requires room for two bytes, so a putchar after
 * it returns true cannot block. */
bool com_putchar_ready(void);

// Telnet console server settings
void com_telnet_task(void);

#define COM_TELNET_KEY_SIZE 33
int com_telnet_port_response(char *buf, size_t buf_size, int state, unsigned width);
int com_telnet_key_response(char *buf, size_t buf_size, int state, unsigned width);

#define COM_TELNET_CONFIG_PORT CONFIG_INT(O, com_telnet, port, uint16_t, 23, \
    nul_check, nul_apply, STR_PORT, com_telnet_port_response, STR_HELP_SET_PORT, NULL)
#define COM_TELNET_CONFIG_KEY CONFIG_STR(A, com_telnet, key, COM_TELNET_KEY_SIZE, "", \
    nul_check, nul_apply, STR_KEY, com_telnet_key_response, STR_HELP_SET_KEY, NULL)
#define COM_TELNET_DRIVER DRIVER(nul_init, com_telnet_task, nul_task, nul_run, \
    nul_stop, nul_break, COM_TELNET_CONFIG_PORT, COM_TELNET_CONFIG_KEY, nul_sst)

__printflike(1, 2) int com_printf_utf8(const char *utf8_fmt, ...);

size_t com_uart_read(char *buf, size_t length);
int com_uart_peek(void);
void com_uart_clear(void);
#define COM_UART_SOURCE {.read = com_uart_read, .peek = com_uart_peek, .clear = com_uart_clear, .dwell_us = COM_WIRE_DWELL_US}

/* Stop and break hooks run in reverse order, so placing COM_DRIVER early
 * makes com_stop write the terminal reset after the stop hooks of the rows
 * that follow it, and com_break write its newline after anything their break
 * hooks print. */
#define COM_DRIVER DRIVER(com_init, com_task, nul_task, com_run, com_stop, com_break, nul_config, nul_config, nul_sst)

#endif /* _RIA_SYS_COM_H_ */
