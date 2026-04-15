/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_COM_H_
#define _RIA_SYS_COM_H_

/* UART driver.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define COM_UART uart1
#define COM_UART_BAUD_RATE 115200
#define COM_UART_TX_PIN 4
#define COM_UART_RX_PIN 5

/* Main events
 */

void com_init(void);
void com_task(void);

// The '\a' BEL alert
bool com_get_bel(void);
void com_set_bel(bool value);

/* TX — for tee to call
 */

bool com_tx_writable(void);
void com_tx_write(char ch);
void com_pump(void);
void com_flush(void);

/* RX — for tee to call
 */

int com_rx(char *buf, int length);

#endif /* _RIA_SYS_COM_H_ */
