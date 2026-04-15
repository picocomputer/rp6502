/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_COM_H_
#define _RIA_SYS_COM_H_

/* Console I/O multiplexer and UART driver.
 * TX fan-out to UART and REM (telnet).
 * RX merge from UART, keyboard, and remote.
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

/* TX — console output
 */

#define COM_BUF_SIZE 32
extern volatile uint8_t com_buf[COM_BUF_SIZE];
extern volatile size_t com_head;
extern volatile size_t com_tail;

// Ensure putchar will not block even with a newline expansion
static inline bool com_putchar_ready(void)
{
    return (
        (((com_head + 1) % COM_BUF_SIZE) != com_tail) &&
        (((com_head + 2) % COM_BUF_SIZE) != com_tail));
}

// Ensure space for com_write()
static inline bool com_writable(void)
{
    return (((com_head + 1) % COM_BUF_SIZE) != com_tail);
}

// Bypasses Pico SDK stdout newline expansion
static inline void com_write(char ch)
{
    com_head = (com_head + 1) % COM_BUF_SIZE;
    com_buf[com_head] = ch;
}

/* RX — console input
 */

// 1-byte message queue to the RIA action loop. -1 = empty
extern volatile int com_rx_char;

#endif /* _RIA_SYS_COM_H_ */
