/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The console part of core/com/manifold.h on the RIA. Output goes out the UART,
 * over PIX to a connected VGA, and to telnet, and the UART is a source of the
 * console. It is also the SDK's stdio driver, and a break on the UART line
 * breaks to the monitor, as Ctrl-Alt-Delete does. */

#ifndef _RIA_COM_MANIFOLD_H_
#define _RIA_COM_MANIFOLD_H_

#include "core/com/manifold.h"

#define MANIFOLD_UART uart1
#define MANIFOLD_UART_BAUD_RATE 115200
#define MANIFOLD_UART_TX_PIN 4
#define MANIFOLD_UART_RX_PIN 5

/* Main events
 */

void manifold_init(void);
void manifold_task(void);

void manifold_uart_drain_rx(void);

/* Stop hooks run in reverse order, so placing MANIFOLD_UART_DRIVER early makes
 * its manifold_flush send everything the other rows' stop hooks print before
 * the display reset that follows a stop. */
#define MANIFOLD_UART_DRIVER DRIVER( \
    INIT(manifold_init)              \
    TASK_ALWAYS(manifold_task)       \
    STOP(manifold_flush))

#endif /* _RIA_COM_MANIFOLD_H_ */
