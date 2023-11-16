/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _COM_H_
#define _COM_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Kernel events
 */

void com_task(void);
void com_init(void);
void com_reset(void);
void com_reclock(void);

// Blocks until all buffers empty.
// This is called before a clock change.
// It shouldn't be used elsewhere.
void com_flush(void);

// Both types of reads guarantee this callback unless a
// break event happens. Timeout is true when input is idle too long.
// Requesting a timeout of 0 ms will disable the idle timer.
typedef void (*com_read_callback_t)(bool timeout, const char *buf, size_t length);

// Prepare to receive binary data of a known size.
void com_read_binary(uint32_t timeout_ms, com_read_callback_t callback, uint8_t *buf, size_t size);

// Prepare the line editor. The com module can read entire lines
// of input with basic editing on ANSI terminals.
void com_read_line(uint32_t timeout_ms, com_read_callback_t callback, size_t size, uint32_t ctrl_bits);

extern volatile size_t com_tx_tail;
extern volatile size_t com_tx_head;
extern volatile uint8_t com_tx_buf[32];
#define COM_TX_BUF(pos) com_tx_buf[(pos) & 0x1F]

static inline bool com_tx_writable(void)
{
    return (((com_tx_head + 1) & 0x1F) != (com_tx_tail & 0x1F));
}

static inline void com_tx_write(char ch)
{
    COM_TX_BUF(++com_tx_head) = ch;
}

#endif /* _COM_H_ */
