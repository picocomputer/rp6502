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
#include "core/com.h"

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
void com_telnet_load_port(const char *str);
void com_telnet_load_key(const char *str);
bool com_telnet_set_port(uint16_t port);
bool com_telnet_set_key(const char *key);
uint16_t com_telnet_get_port(void);
const char *com_telnet_get_key(void);

// Console TX, UTF-8 formatted. Pico only: the monitor, BLE and the
// network status lines are the callers.
__printflike(1, 2) int com_printf_utf8(const char *utf8_fmt, ...);
int com_vprintf_utf8(const char *utf8_fmt, va_list va);
__printflike(3, 4) int com_snprintf_utf8(char *dst, size_t dst_size,
                                         const char *utf8_fmt, ...);
int com_vsnprintf_utf8(char *dst, size_t dst_size,
                       const char *utf8_fmt, va_list va);

#endif /* _RIA_SYS_COM_H_ */
