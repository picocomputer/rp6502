/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_MEM_H_
#define _RIA_SYS_MEM_H_

/* The monitor's buffer and the transfer machinery over it.
 */

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

/* This driver's row in a machine's driver list; see core/sys/driver.h. */
#define MEM_DRIVER DRIVER(nul_init, nul_task, mem_task, nul_run, nul_stop, mem_break, nul_config, nul_config)

#endif /* _RIA_SYS_MEM_H_ */
