/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_REM_H_
#define _RIA_SYS_REM_H_

/* Remote console server.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void rem_init(void);
void rem_task(void);

/* Console tee
 */

#define REM_TX_BUF_SIZE 256
extern char rem_tx_buf[REM_TX_BUF_SIZE];
extern volatile size_t rem_tx_head;
extern volatile size_t rem_tx_tail;

static inline bool rem_putchar_ready(void)
{
    return (
        (((rem_tx_head + 1) % REM_TX_BUF_SIZE) != rem_tx_tail) &&
        (((rem_tx_head + 2) % REM_TX_BUF_SIZE) != rem_tx_tail));
}

static inline bool rem_tx_writable(void)
{
    return ((rem_tx_head + 1) % REM_TX_BUF_SIZE) != rem_tx_tail;
}

static inline void rem_putc(char ch)
{
    rem_tx_head = (rem_tx_head + 1) % REM_TX_BUF_SIZE;
    rem_tx_buf[rem_tx_head] = ch;
}

void rem_pump(void);
void rem_flush(void);
int rem_rx(char *buf, int length);

/* Configuration
 */

void rem_load_port(const char *str);
void rem_load_key(const char *str);
bool rem_set_port(uint32_t port);
bool rem_set_key(const char *key);
uint16_t rem_get_port(void);
const char *rem_get_key(void);

#endif /* _RIA_SYS_REM_H_ */
