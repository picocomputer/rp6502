/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_TEE_H_
#define _RIA_SYS_TEE_H_

/* Console I/O multiplexer.
 * TX fan-out to COM (UART) and REM (telnet).
 * RX merge from UART, keyboard, and remote.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void tee_init(void);
void tee_task(void);
void tee_run(void);
void tee_stop(void);

/* TX — console output
 */

#define TEE_BUF_SIZE 32
extern volatile uint8_t tee_buf[TEE_BUF_SIZE];
extern volatile size_t tee_head;
extern volatile size_t tee_tail;

// Ensure putchar will not block even with a newline expansion
static inline bool tee_putchar_ready(void)
{
    return (
        (((tee_head + 1) % TEE_BUF_SIZE) != tee_tail) &&
        (((tee_head + 2) % TEE_BUF_SIZE) != tee_tail));
}

// Ensure space for tee_write()
static inline bool tee_writable(void)
{
    return (((tee_head + 1) % TEE_BUF_SIZE) != tee_tail);
}

// Bypasses Pico SDK stdout newline expansion
static inline void tee_write(char ch)
{
    tee_head = (tee_head + 1) % TEE_BUF_SIZE;
    tee_buf[tee_head] = ch;
}

/* RX — console input
 */

// 1-byte message queue to the RIA action loop. -1 = empty
extern volatile int tee_rx_char;

#endif /* _RIA_SYS_TEE_H_ */
