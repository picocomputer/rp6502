/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_SYS_COM_H_
#define _VGA_SYS_COM_H_

/* Communications switchboard
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// UART connected to RIA
#define COM_UART_TX_PIN 4
#define COM_UART_RX_PIN 5
#define COM_UART_INTERFACE uart1
#define COM_UART_BAUDRATE 115200

// IN Buffering is also 32 byte UART FIFO
#define COM_IN_BUF_SIZE 16
// OUT Buffer matches full speed USB BULK_PACKET_SIZE
#define COM_OUT_BUF_SIZE 64

/* Main events
 */

void com_init(void);
void com_task(void);
void com_pre_reclock(void);
void com_post_reclock(void);

// USB CDC controls UART break
void com_set_uart_break(bool en);

// IN is sourced by USB CDC
// IN is sunk here to UART
size_t com_in_free(void);
bool com_in_empty(void);
void com_in_write(char ch);
void com_in_write_ansi_CPR(int row, int col);

// OUT is sourced here from UART
// OUT is sourced from PIX $F:03
// OUT is sunk here to stdio(term)
// OUT is sunk by USB CDC
bool com_out_empty(void);
void com_out_write(char ch);
char com_out_peek(void);
char com_out_read(void);

#endif /* _VGA_SYS_COM_H_ */
