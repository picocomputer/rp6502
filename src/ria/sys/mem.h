/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_MEM_H_
#define _RIA_SYS_MEM_H_

/* What the monitor moves things around with. The memory the machine itself
 * sees -- xram, the xstack, the register window -- is core/mem.h; this is the
 * Pico's own buffer and the transfer machinery over it. */

#include "core/mem.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Misc memory buffer for moving things around.
// 6502 <-> RAM, USB <-> RAM, UART <-> RAM, etc.
// Also used as a littlefs buffer for read/write.
#define MBUF_SIZE 1024
extern uint8_t mbuf[];
extern size_t mbuf_len;

// Read memory buffer from stdio
typedef void (*mem_read_callback_t)(bool timeout);
void mem_task(void);
void mem_break(void);
void mem_read_mbuf(uint32_t timeout_ms, mem_read_callback_t callback, size_t size);

// CRC-32/ISO-HDLC (zlib). mem_crc32(0, buf, len) is the one-shot value; chain by
// feeding a prior result back as crc.
uint32_t mem_crc32(uint32_t crc, const void *buf, size_t len);

#endif /* _RIA_SYS_MEM_H_ */
