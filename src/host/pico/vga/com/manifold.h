/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The terminal part of core/com/manifold.h on the VGA. What the RIA sends, on
 * the UART or over PIX as UART_TX, goes through the SDK's stdio to the
 * terminal and the USB console; what it sends over PIX as TERM_TX goes to the
 * terminal alone. What the USB console sends, and the terminal's replies, go
 * to the RIA on the UART. */

#ifndef _VGA_COM_MANIFOLD_H_
#define _VGA_COM_MANIFOLD_H_

#include "core/sys/driver.h"
#include "core/com/manifold.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// UART connected to RIA
#define MANIFOLD_UART_TX_PIN 4
#define MANIFOLD_UART_RX_PIN 5
#define MANIFOLD_UART_INTERFACE uart1
#define MANIFOLD_UART_BAUDRATE 115200

// IN buffering adds to the 32-byte UART FIFO.
// This doesn't need to be large.
#define MANIFOLD_IN_BUF_SIZE 16
// OUT buffer is generous to prevent data loss on
// forwarded USB ports (usbipd). 32=bad 64=ok 128=safe
#define MANIFOLD_OUT_BUF_SIZE 128

/* Main events
 */

void manifold_init(void);
void manifold_task(void);
void manifold_pre_reclock(void);
void manifold_post_reclock(void);

// USB CDC controls UART break
void manifold_set_uart_break(bool en);

// IN is sourced by USB CDC
// IN is sunk here to UART
size_t manifold_in_free(void);
bool manifold_in_empty(void);
void manifold_in_write(char ch);

// OUT is sourced here from UART
// OUT is sourced from PIX $F:03
// OUT is sunk to term
// OUT is sunk by USB CDC
bool manifold_out_empty(void);
bool manifold_out_full(void);
char manifold_out_peek(void);
char manifold_out_read(void);

/* One byte from PIX $F:05 to the terminal alone, as it is. */
void manifold_term_putc(char ch);

#define MANIFOLD_UART_DRIVER DRIVER(INIT(manifold_init))

#endif /* _VGA_COM_MANIFOLD_H_ */
