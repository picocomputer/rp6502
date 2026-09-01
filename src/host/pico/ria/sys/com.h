/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_COM_H_
#define _RIA_SYS_COM_H_

/* COnsole Manifold and UART driver.
 * TX fan-out to UART and REM (telnet).
 * RX merge from UART, keyboard, and remote.
 */

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

// Telnet console server settings
void com_telnet_task(void);

#define COM_TELNET_KEY_SIZE 33
int com_telnet_port_response(char *buf, size_t buf_size, int state, unsigned width);
int com_telnet_key_response(char *buf, size_t buf_size, int state, unsigned width);

/* Telnet is a console source with two settings of its own, so it is a row
 * rather than a call inside com_task. A machine without a radio does not
 * roster it and does not keep its settings either. */
#define COM_TELNET_CONFIG_PORT CONFIG_INT(O, com_telnet, port, uint16_t, 23, \
    nul_check, nul_apply, STR_PORT, com_telnet_port_response, STR_HELP_SET_PORT, NULL)
#define COM_TELNET_CONFIG_KEY CONFIG_STR(A, com_telnet, key, COM_TELNET_KEY_SIZE, "", \
    nul_check, nul_apply, STR_KEY, com_telnet_key_response, STR_HELP_SET_KEY, NULL)
#define COM_TELNET_DRIVER DRIVER(nul_init, com_telnet_task, nul_task, nul_run, \
    nul_stop, nul_break, COM_TELNET_CONFIG_PORT, COM_TELNET_CONFIG_KEY)

/* Console TX for UTF-8 source text, converted to the code page on the way
 * out. The monitor's prompts and the UF2 progress line are the callers;
 * oem_snprintf is the same thing into a buffer. */
__printflike(1, 2) int com_printf_utf8(const char *utf8_fmt, ...);

/* This machine's console row, early because everything after it may print.
 * Early is also what its stop and its break want: both walk backward, so a
 * row near the front is torn down near the last -- com_stop writing the reset
 * after the other stops, com_break its newline after whatever they printed. */
#define COM_DRIVER DRIVER(com_init, com_task, nul_task, com_run, com_stop, com_break, nul_config, nul_config)

#endif /* _RIA_SYS_COM_H_ */
