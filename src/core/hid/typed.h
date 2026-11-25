/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The typed queue, the keyboard's one queue: everything the keyboard and
 * the console's mouse send, each in its place, in the order it was
 * pressed. The keyboard row of RP6502_COM_SOURCES reads it. It holds
 * TYPED_QUEUE_SIZE entries, which machine.h sets. An entry is a byte, or
 * one of the things that have no bytes to wait there as:
 *
 * - a paste, which names its buffer (clip.h). The row types its text when
 *   its turn comes, straight out of the buffer as it is then, between
 *   ESC [ 200 ~ and ESC [ 201 ~ when the console's bracketed paste (?2004)
 *   is set as the text starts. An empty buffer sends nothing, not even
 *   brackets. Pastes wait their turn, and each is typed whole;
 * - the copy key, a cut key, whose own bytes follow its mark, and the
 *   mouse in the line the line editor is editing (core/com/rln.h). Only
 *   the line editor takes these. With no line being read, one that
 *   reaches the front is dropped, and a cut key's bytes are then typed as
 *   any key's are.
 *
 * So a reader of bytes is never held up by an entry nobody will take, and
 * what the line editor takes acts between the key before it and the key
 * after it. */

#ifndef _CORE_HID_TYPED_H_
#define _CORE_HID_TYPED_H_

#include "core/sys/sst.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* An entry: its kind in the top four bits, and twelve bits the kind
 * reads. A byte's entry is the byte. */
typedef uint16_t typed_entry_t;
enum
{
    TYPED_BYTE,    /* the byte */
    TYPED_VOID,    /* nothing: an entry taken back while it waited */
    TYPED_PASTE,   /* the source, and TYPED_PASTE_PLAIN */
    TYPED_COPY,    /* the copy key */
    TYPED_CUT,     /* a cut key: how many of its bytes follow */
    TYPED_PRESS,   /* the mouse in the line: these four are rln.c's */
    TYPED_HELD,
    TYPED_DRAG,
    TYPED_RELEASE,
};
#define TYPED_ENTRY(kind, arg) ((typed_entry_t)((kind) << 12 | (arg)))
#define TYPED_KIND(entry) ((entry) >> 12)

/* A paste's source is a buffer of clip.h, in the low two bits. Plain text
 * is typed as written, never bracketed. */
#define TYPED_PASTE_SOURCE 3
#define TYPED_PASTE_PLAIN 4

/* Bytes, all of a push or none of it, so a key's sequence is never cut. A
 * Ctrl-C in what is pushed raises the SIGINT, even when the queue is full
 * and the bytes are dropped, ends the paste being typed, with its closing
 * bracket, and drops every paste still waiting. Plain text is typing, as
 * keys are, and a Ctrl-C leaves it. */
void typed_push(const char *s, size_t n);

/* One entry that is no byte: false, with nothing queued, when the queue is
 * full. */
bool typed_put(typed_entry_t entry);

/* A cut key: its mark and the n bytes the key types, all or none. */
void typed_cut(const char *s, size_t n);

/* The mouse's drag in the line, from index held to index to: a held entry
 * and a drag entry. They replace the newest pair when nothing was queued
 * since, so motion merges. Its caller has seen to the room for two. */
void typed_drag(uint8_t held, uint8_t to);

/* An entry of the line editor's is at the front: no paste is being typed
 * and every byte before it has been read. typed_take takes it, a cut key's
 * bytes with its mark. */
bool typed_entry(void);
bool typed_take(typed_entry_t *entry);

/* The mouse's entries still waiting are taken back. */
void typed_void_mouse(void);

/* The machine buffer which changed: a paste being typed from it ends here,
 * with its closing bracket. */
void typed_clip_changed(uint8_t which);

/* The room left, in entries. */
size_t typed_free(void);

/* The keyboard row's read and peek (com.h): bytes, a paste's among them. */
size_t typed_read(char *buf, size_t length);
int typed_peek(size_t offset);

/* Everything queued is dropped, the paste being typed too. It is the
 * keyboard row's clear on a machine whose host does its typing;
 * keymap_abandon is the row's clear where the machine types through its
 * own layouts. */
void typed_clear(void);
#define TYPED_COM_SOURCE {.read = typed_read, .peek = typed_peek, \
    .clear = typed_clear, .dwell_us = COM_DWELL_US, .size = TYPED_QUEUE_SIZE}

extern const sst_table_t typed_sst;

/* After CLIP_DRIVER, whose buffers a paste being typed is checked against
 * as it loads. */
#define TYPED_DRIVER DRIVER(     \
    RESET(typed_clear)           \
    SST("TYPD", typed))

#endif /* _CORE_HID_TYPED_H_ */
