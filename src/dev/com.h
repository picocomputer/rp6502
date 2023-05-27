/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _COM_H_
#define _COM_H_

#define RIA_UART uart1
#define RIA_UART_BAUD_RATE 115200
#define RIA_UART_TX_PIN 4
#define RIA_UART_RX_PIN 5

extern volatile int ria_uart_rx_char;

void com_task();
void com_init();
void com_reset();
void com_preclock();
void com_reclock();

#endif /* _COM_H_ */
