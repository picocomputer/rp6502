/*
 * Copyright (c) 2023 Rumbledethumps
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

/* Kernel events
 */

void com_task(void);
void com_init(void);
void com_run(void);
void com_stop(void);
void com_pre_reclock(void);
void com_post_reclock(void);

/* Expose the internals here because ria.c needs direct access
 */

// 1-byte message queue to the RIA action loop.
extern volatile int com_rx_char;

extern volatile size_t com_tx_tail;
extern volatile size_t com_tx_head;
extern volatile uint8_t com_tx_buf[32];

// Ensure space for newline expansion
static inline bool com_tx_printable(void)
{
    return (
        (((com_tx_head + 1) & 0x1F) != (com_tx_tail & 0x1F)) &&
        (((com_tx_head + 2) & 0x1F) != (com_tx_tail & 0x1F)));
}

// Ensure space for com_tx_write()
static inline bool com_tx_writable(void)
{
    return (((com_tx_head + 1) & 0x1F) != (com_tx_tail & 0x1F));
}

static inline void com_tx_write(char ch)
{
    com_tx_buf[++com_tx_head & 0x1F] = ch;
}

#endif /* _RIA_SYS_COM_H_ */
