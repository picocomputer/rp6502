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
void com_pre_reclock(void);
void com_post_reclock(void);

// Blocks until all buffers empty. Use sparingly.
void com_flush(void);

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

#endif /* _COM_H_ */
