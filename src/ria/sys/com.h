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
#include <hardware/sync.h>

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

// com_tx_buf is the core-0-only TX ring. Producers (stdio, std_tty_write)
// and consumer (com_tx_fanout) all run on the core-0 main loop, so the
// SPSC protocol is serialized naturally; no lock, no __dmb() needed.
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

// Bypasses Pico SDK stdout newline expansion. Caller must have checked
// com_writable() first. Core-0 producers only.
static inline void com_write(char ch)
{
    size_t next = (com_tx_head + 1) % COM_TX_BUF_SIZE;
    com_tx_buf[next] = (uint8_t)ch;
    com_tx_head = next;
}

// com_act_tx_buf is the core-1 act_loop TX ring, drained by com_tx_fanout
// on core 0. Single producer (act_loop), single consumer (com_tx_fanout);
// __dmb() publishes the slot before the head so the cross-core reader
// cannot observe a new head with a stale slot.
#define COM_ACT_TX_BUF_SIZE 32
extern volatile uint8_t com_act_tx_buf[COM_ACT_TX_BUF_SIZE];
extern volatile size_t com_act_tx_head;
extern volatile size_t com_act_tx_tail;

static inline bool com_act_writable(void)
{
    return (((com_act_tx_head + 1) % COM_ACT_TX_BUF_SIZE) != com_act_tx_tail);
}

// Caller (act_loop) must have checked com_act_writable() first.
static inline void com_act_write(char ch)
{
    size_t next = (com_act_tx_head + 1) % COM_ACT_TX_BUF_SIZE;
    com_act_tx_buf[next] = (uint8_t)ch;
    __dmb();
    com_act_tx_head = next;
}

/* RX — console input
 */

// com_rx_char is the cross-core single-char handoff slot.
// -1 => empty, else 0..255.
// Producer: com_task on core 0 publishes a non-negative value when the
// slot is empty (ordered with __dmb()).
// Consumers: act_loop on core 1 (primary, serves 6502 0xFFE2 reads) or
// com_stdio_in_chars on core 0 (steal path when monitor is active).
// The application is responsible for not mixing readers between the
// monitor and the 6502; under that invariant the dual-consumer clear
// path cannot race.
extern volatile int com_rx_char;

#endif /* _RIA_SYS_COM_H_ */
