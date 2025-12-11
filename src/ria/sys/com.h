/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_COM_H_
#define _RIA_SYS_COM_H_

/* Communications switchboard.
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

void com_task(void);
void com_init(void);
void com_run(void);
void com_stop(void);
void com_pre_reclock(void);
void com_post_reclock(void);

/* Expose the internals here because ria.c needs direct access
 */

// 1-byte message queue to the RIA action loop. -1 = empty
extern volatile int com_rx_char;

#define COM_TX_BUF_SIZE 32
extern volatile uint8_t com_tx_buf[COM_TX_BUF_SIZE];
extern volatile size_t com_tx_tail;
extern volatile size_t com_tx_head;

// Ensure putchar will not block even with a newline expansion
static inline bool com_putchar_ready(void)
{
    return (
        (((com_tx_head + 1) % COM_TX_BUF_SIZE) != com_tx_tail) &&
        (((com_tx_head + 2) % COM_TX_BUF_SIZE) != com_tx_tail));
}

// Ensure space for com_tx_write()
static inline bool com_tx_writable(void)
{
    return (((com_tx_head + 1) % COM_TX_BUF_SIZE) != com_tx_tail);
}

// Bypasses Pico SDK stdout newline expansion
static inline void com_tx_write(char ch)
{
    com_tx_head = (com_tx_head + 1) % COM_TX_BUF_SIZE;
    com_tx_buf[com_tx_head] = ch;
}

#endif /* _RIA_SYS_COM_H_ */
