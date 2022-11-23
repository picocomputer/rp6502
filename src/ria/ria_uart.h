/*
 * Copyright (c) 2022 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_UART_H_
#define _RIA_UART_H_

#define RIA_UART uart1
#define RIA_UART_BAUD_RATE 115200
#define RIA_UART_TX_PIN 4
#define RIA_UART_RX_PIN 5

extern volatile int ria_uart_rx_char;

void ria_uart_task();
void ria_uart_init();
void ria_uart_flush();

#endif /* _RIA_UART_H_ */
