/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The paste stage: a block of text typed whole, in front of the typed
 * queue. The keyboard row of RP6502_COM_SOURCES reads through it.
 *
 * What the queue held when a paste starts is read first, then the paste,
 * then what was typed while it went in: those keys wait in the queue behind
 * it, and once the queue is full the rest are dropped. The text is staged a
 * few bytes at a time, as code page bytes, by paste_task. A Ctrl-C typed
 * during a paste still raises the SIGINT at once, and ends the paste.
 *
 * A paste that the console's bracketed paste (?2004) was set for when it
 * started comes between ESC [ 200 ~ and ESC [ 201 ~. The closing bracket is
 * sent even when the paste is cut short. A paste with no text, of an empty
 * buffer or of host text with nothing but control characters, sends no
 * brackets either. A paste started while another is going in cuts that one
 * short and follows it, once all of that one, its closing bracket too, has
 * been read. */

#ifndef _CORE_HID_PASTE_H_
#define _CORE_HID_PASTE_H_

#include "core/com/com.h"
#include "core/hid/typed.h"
#include "core/sys/driver.h"
#include "core/sys/sst.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A source of text outside the machine, the host's: it writes up to room
 * code page bytes, room at least one, and returns how many, or none once
 * it has no more. Host text is never saved, and every load ends it; a save
 * keeps only the brackets the program is owed. */
typedef size_t (*paste_fill_t)(char *dst, size_t room);
void paste_host(paste_fill_t fill, bool bracketed);

/* The host's text that fill reads is going: a paste of it ends here, with
 * its closing bracket, before fill would read what is no longer there. */
void paste_host_cut(paste_fill_t fill);

/* The machine's buffer which (clip.h), bracketed as the console's ?2004
 * says, from its start. It is machine state, saved with the rest. */
void paste_clip(uint8_t which);

/* The buffer which changed: a paste from it ends here. */
void paste_clip_changed(uint8_t which);

/* The keyboard row's read and peek (com.h). */
size_t paste_read(char *buf, size_t length);
int paste_peek(size_t offset);

/* A paste is going in: text, or a bracket, is still to be read. */
bool paste_busy(void);

/* A paste still has text to stage, or waits to start, so its source is
 * still read. */
bool paste_staging(void);

/* The rest of the paste goes, and its closing bracket comes. A paste that
 * waits to start goes with no brackets. */
void paste_cut(void);

/* The paste goes, all of it. */
void paste_cancel(void);

/* The keyboard row's clear on a machine whose host does its typing: the
 * paste and the typed queue. keymap_abandon is the row's clear where the
 * machine types through its own layouts. */
void paste_clear(void);
#define PASTE_COM_SOURCE {.read = paste_read, .peek = paste_peek, \
    .clear = paste_clear, .dwell_us = COM_DWELL_US, .size = TYPED_QUEUE_SIZE}

void paste_task(void);

extern const sst_table_t paste_sst;

#define PASTE_DRIVER DRIVER(     \
    TASK_PUMPED(paste_task)      \
    RESET(paste_cancel)          \
    SST("PAST", paste))

#endif /* _CORE_HID_PASTE_H_ */
