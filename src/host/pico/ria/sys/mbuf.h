/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_MBUF_H_
#define _RIA_SYS_MBUF_H_

/* The monitor's buffer and the transfer machinery over it.
 */

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
typedef void (*mbuf_read_callback_t)(bool timeout);
void mbuf_task(void);
void mbuf_break(void);
void mbuf_read(uint32_t timeout_ms, mbuf_read_callback_t callback, size_t size);

/* This driver's row in a machine's driver list; see core/sys/driver.h. */
#define MBUF_DRIVER DRIVER(nul_init, nul_task, mbuf_task, nul_run, nul_stop, mbuf_break, nul_config, nul_config)

#endif /* _RIA_SYS_MBUF_H_ */
