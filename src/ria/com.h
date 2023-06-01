/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _COM_H_
#define _COM_H_

#include "vga/ansi.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define COM_UART uart1
#define COM_UART_BAUD_RATE 115200
#define COM_UART_TX_PIN 4
#define COM_UART_RX_PIN 5

void com_task();
void com_init();
void com_reset();
void com_flush();
void com_reclock();

// Writes in stdout style. Non-blocking.
// Fills UART buffer then stops.
size_t com_write(char *ptr, size_t count);

typedef void (*com_read_callback_t)(bool timeout, size_t length);

// Redirect UART RX to mbuf for binary data.
void com_read_binary(uint8_t *buf, size_t size, uint32_t timeout_ms, com_read_callback_t callback);

// Redirect UART RX to a line editor.
void com_read_line(char *buf, size_t size, uint32_t timeout_ms, com_read_callback_t callback);

#endif /* _COM_H_ */
