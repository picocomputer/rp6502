/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_COM_HW_H_
#define _RIA_SYS_COM_HW_H_

/* COnsole Manifold and UART driver, hardware-only surface.
 * TX fan-out to UART and REM (telnet).
 * RX merge from UART, keyboard, and remote.
 */

#include "sys/com.h"

#define COM_UART uart1
#define COM_UART_BAUD_RATE 115200
#define COM_UART_TX_PIN 4
#define COM_UART_RX_PIN 5

/* Main events
 */

void com_init(void);
void com_task(void);
void com_stop(void);
void com_break(void);

// Telnet console server settings
void com_tel_load_port(const char *str);
void com_tel_load_key(const char *str);
bool com_tel_set_port(uint16_t port);
bool com_tel_set_key(const char *key);
uint16_t com_tel_get_port(void);
const char *com_tel_get_key(void);

#endif /* _RIA_SYS_COM_HW_H_ */
