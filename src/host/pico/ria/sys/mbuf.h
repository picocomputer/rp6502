/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_MBUF_H_
#define _RIA_SYS_MBUF_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "core/com/com.h"
#include "core/sys/buf1k.h"

// What the monitor keeps beside buf1k (core/sys/buf1k.h): how many bytes of
// it are in use, for 6502 <-> RAM, USB <-> RAM, UART <-> RAM and the like.
extern size_t mbuf_len;

// Read buf1k from stdio, from src alone or from COM_SOURCE_ANY.
typedef void (*mbuf_read_callback_t)(bool timeout);
void mbuf_task(void);
void mbuf_break(void);
void mbuf_read(com_source_t src, uint32_t timeout_ms, mbuf_read_callback_t callback,
               size_t size);

#define MBUF_DRIVER DRIVER(TASK_FATFS(mbuf_task) BREAK(mbuf_break))

#endif /* _RIA_SYS_MBUF_H_ */
