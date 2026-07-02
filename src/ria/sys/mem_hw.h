/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_MEM_HW_H_
#define _RIA_SYS_MEM_HW_H_

/* The mbuf transfer engine, hardware-only surface.
 * 6502 <-> RAM, USB <-> RAM, UART <-> RAM, etc.
 * Also used as a littlefs buffer for read/write.
 */

#include "sys/mem.h"

#define MBUF_SIZE 1024
extern uint8_t mbuf[];
extern size_t mbuf_len;

// Read memory buffer from stdio
typedef void (*mem_read_callback_t)(bool timeout);
void mem_task(void);
void mem_break(void);
void mem_read_mbuf(uint32_t timeout_ms, mem_read_callback_t callback, size_t size);

#endif /* _RIA_SYS_MEM_HW_H_ */
