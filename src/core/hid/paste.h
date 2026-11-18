/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* A block of text typed into the manifold's keyboard ring, on a machine whose
 * host can hand one over. */

#ifndef _CORE_HID_PASTE_H_
#define _CORE_HID_PASTE_H_

#include "core/com/manifold.h"
#include "core/sys/driver.h"
#include <stdbool.h>

/* Type a block of UTF-8 as keystrokes. Every control byte, CR and LF
 * included, is sent as written. paste_task paces the delivery against the
 * room left in the ring, so text longer than the ring cannot lose bytes. A
 * new call replaces whatever is still being delivered. */
void paste_start(const char *utf8);
void paste_cancel(void);
bool paste_busy(void);

/* Clear the keyboard source: the ring and the rest of the paste. */
void paste_clear(void);
#define PASTE_COM_SOURCE {.read = manifold_keyboard_read, .peek = manifold_keyboard_peek, \
    .clear = paste_clear, .dwell_us = COM_DWELL_US, .size = MANIFOLD_RING_SIZE}

void paste_task(void);

#define PASTE_DRIVER DRIVER(TASK_PUMPED(paste_task))

#endif /* _CORE_HID_PASTE_H_ */
