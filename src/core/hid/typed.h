/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The queue of typed bytes: what the keyboard and pastes type, waiting for
 * the keyboard row of RP6502_COM_SOURCES to read it. It holds
 * TYPED_QUEUE_SIZE bytes, which machine.h sets. */

#ifndef _CORE_HID_TYPED_H_
#define _CORE_HID_TYPED_H_

#include "core/sys/sst.h"
#include <stddef.h>

/* A Ctrl-C in what is pushed raises the SIGINT, even when the queue is full
 * and the bytes are dropped. The queue takes all of a push or none of it, so
 * a key's sequence is never cut. */
void typed_push(const char *s, size_t n);

/* The room left. */
size_t typed_free(void);

size_t typed_read(char *buf, size_t length);
int typed_peek(size_t offset);
void typed_clear(void);

void typed_reset(void);
extern const sst_table_t typed_sst;

#define TYPED_DRIVER DRIVER(     \
    RESET(typed_reset)           \
    SST("TYPD", typed))

#endif /* _CORE_HID_TYPED_H_ */
