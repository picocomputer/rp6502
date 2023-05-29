/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _COM_H_
#define _COM_H_

#include <stddef.h>
#include <stdint.h>

#define COM_UART uart1
#define RIA_UART_BAUD_RATE 115200
#define RIA_UART_TX_PIN 4
#define RIA_UART_RX_PIN 5

void com_task();
void com_init();
void com_reset();
void com_preclock();
void com_reclock();

// Writes in stdout style. Non-blocking.
// Fills UART buffer then stops.
size_t com_write(char *ptr, size_t count);

// Redirect UART RX bulk data to mbuf.
// When timeout, mbuf_len will be < length in callback.
void com_capture_mbuf(void (*callback)(void), size_t length, uint32_t timeout_ms);

#endif /* _COM_H_ */
