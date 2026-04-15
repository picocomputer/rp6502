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

#define COM_TX_BUF_SIZE 32
extern volatile uint8_t com_tx_buf[COM_TX_BUF_SIZE];
extern volatile size_t com_tx_head;
extern volatile size_t com_tx_tail;

// Ensure putchar will not block even with a newline expansion
static inline bool com_putchar_ready(void)
{
    return (
        (((com_tx_head + 1) % COM_TX_BUF_SIZE) != com_tx_tail) &&
        (((com_tx_head + 2) % COM_TX_BUF_SIZE) != com_tx_tail));
}

// Ensure space for com_write()
static inline bool com_writable(void)
{
    return (((com_tx_head + 1) % COM_TX_BUF_SIZE) != com_tx_tail);
}

// Bypasses Pico SDK stdout newline expansion
static inline void com_write(char ch)
{
    com_tx_head = (com_tx_head + 1) % COM_TX_BUF_SIZE;
    com_tx_buf[com_tx_head] = ch;
}

/* RX — console input
 */

#define COM_RX_BUF_SIZE 32
extern volatile uint8_t com_rx_buf[COM_RX_BUF_SIZE];
extern volatile size_t com_rx_head;
extern volatile size_t com_rx_tail;

static inline bool com_readable(void)
{
    return com_rx_head != com_rx_tail;
}

static inline char com_read(void)
{
    com_rx_tail = (com_rx_tail + 1) % COM_RX_BUF_SIZE;
    return com_rx_buf[com_rx_tail];
}

#endif /* _RIA_SYS_COM_H_ */
