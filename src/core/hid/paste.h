/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* A block of text typed into the typed queue, on a machine whose host can
 * hand one over. */

#ifndef _CORE_HID_PASTE_H_
#define _CORE_HID_PASTE_H_

#include "core/com/com.h"
#include "core/hid/typed.h"
#include "core/sys/driver.h"
#include "core/sys/sst.h"
#include <stdbool.h>

/* Two ways into the typed queue for a block of UTF-8, both paced by
 * paste_task against the room left in the queue, so text longer than the
 * queue cannot lose bytes. Each replaces whatever is still being delivered,
 * and every savestate load cancels what is. */

/* Paste the host's text: each line end (CRLF, CR or LF) is pressed as
 * Enter, CR, and a program that set bracketed paste (?2004) gets it between
 * ESC [ 200 ~ and ESC [ 201 ~. */
void paste_start(const char *utf8);

/* Type the text exactly as written, never bracketed and with every byte
 * sent as it is, as a script types it. */
void typed_text(const char *utf8);

void paste_cancel(void);
bool paste_busy(void);

/* Clear the keyboard source: the queue and the rest of the paste. */
void paste_clear(void);
#define PASTE_COM_SOURCE {.read = typed_read, .peek = typed_peek, \
    .clear = paste_clear, .dwell_us = COM_DWELL_US, .size = TYPED_QUEUE_SIZE}

void paste_task(void);

extern const sst_table_t paste_sst;

#define PASTE_DRIVER DRIVER(     \
    TASK_PUMPED(paste_task)      \
    RESET(paste_cancel)          \
    SST("PAST", paste))

#endif /* _CORE_HID_PASTE_H_ */
