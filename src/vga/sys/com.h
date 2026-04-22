/*
 * Copyright (c) 2026 Rumbledethumps
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

// IN buffering adds to the 32-byte UART FIFO.
// This doesn't need to be large.
#define COM_IN_BUF_SIZE 16
// OUT buffer is generous to prevent data loss on
// forwarded USB ports (usbipd). 32=bad 64=ok 128=safe
#define COM_OUT_BUF_SIZE 128

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
void com_suppress_term_reply(bool suppress);
void com_in_write_ansi_CPR(unsigned row, unsigned col);
void com_in_write_ansi_DA(void);
void com_in_write_ansi_DSR_ok(void);

// OUT is sourced here from UART
// OUT is sourced from PIX $F:03
// OUT is sunk to term
// OUT is sunk by USB CDC
bool com_out_empty(void);
bool com_out_full(void);
char com_out_peek(void);
char com_out_read(void);

#endif /* _VGA_SYS_COM_H_ */
