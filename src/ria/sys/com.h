/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _COM_H_
#define _COM_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define COM_UART uart1
#define COM_UART_BAUD_RATE 115200
#define COM_UART_TX_PIN 4
#define COM_UART_RX_PIN 5

// Kernel events
void com_task();
void com_init();
void com_reset();
void com_reclock();

// Blocks until all buffers empty.
// This is called before a clock change.
// It shouldn't be used elsewhere.
void com_flush();

// Writes in stdout style. Non-blocking.
// Fills UART buffer then stops.
// Returns number of chars written successfully.
size_t com_write(char *ptr, size_t count);

// Both types of reads guarantee this callback unless a
// break event happens. Timeout is true when input is idle too long.
// Requesting a timeout of 0 ms will disable the idle timer.
typedef void (*com_read_callback_t)(bool timeout, size_t length);

// Prepare to receive binary data of a known size.
void com_read_binary(uint8_t *buf, size_t size, uint32_t timeout_ms, com_read_callback_t callback);

// Prepare the line editor. The com module can read entire lines
// of input with basic editing on ANSI terminals.
void com_read_line(char *buf, size_t size, uint32_t timeout_ms, com_read_callback_t callback);

#endif /* _COM_H_ */
