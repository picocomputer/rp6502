/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The two buffers of copied text, as Linux has them: the primary, which
 * selecting fills and the middle button pastes, and the clipboard, which
 * the copy keys fill and the paste keys paste. The primary is always the
 * machine's. The clipboard is the host's where the host has one
 * (host_clipboard_get), and otherwise the machine's.
 *
 * A machine buffer holds the text of the largest screen, 80 columns by
 * TERM_MAX_HEIGHT rows with a line end after each row. What it holds is
 * chosen by the file the machine builds, as the cell kind is: clip_text.c
 * keeps each character's cell_text word, so a copy to the host keeps the
 * character the glyph shows, and clip_byte.c keeps its code page byte
 * (cell_text_byte), half the size. Both save the machine's buffers and
 * never the host's text.
 *
 * A paste is an entry in the keyboard's queue (typed.h), which reads its
 * buffer when its turn comes, where the text is. So a paste types what
 * the buffer holds then: a cut the line editor takes before it fills the
 * clipboard the paste reads. */

#ifndef _CORE_HID_CLIP_H_
#define _CORE_HID_CLIP_H_

#include "core/hid/typed.h"
#include "core/sys/driver.h"
#include "core/sys/sst.h"
#include "core/term/screen.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* CLIP_HOST is no buffer: it is the host's text a paste types, on a
 * machine that builds clip_text.c (paste_host.h). */
enum
{
    CLIP_PRIMARY,
    CLIP_CLIPBOARD,
    CLIP_COUNT,
    CLIP_HOST = CLIP_COUNT,
};

#define CLIP_SIZE ((80 + 1) * TERM_MAX_HEIGHT)

/* The text of screen s from row y0, column x0 up to row y1, column x1, as
 * screen_copy takes it, into buffer which: the clipboard as UTF-8 to the
 * host's where it has one. A paste being typed from the machine's buffer
 * ends here. */
void clip_copy(uint8_t which, screen_t *s, uint8_t y0, uint8_t x0, uint8_t y1, uint8_t x1);

/* The line editor's text, n code page bytes as it holds them, goes into
 * buffer which, as clip_copy's does. */
void clip_copy_bytes(uint8_t which, const char *bytes, size_t n);

/* Buffer which is pasted: one entry, after everything typed so far. A full
 * queue drops it, as it drops a key. */
static inline void clip_paste(uint8_t which)
{
    typed_put(TYPED_ENTRY(TYPED_PASTE, which));
}

/* What the keyboard's queue asks as a paste's turn comes. The source its
 * text is read from: the buffer the paste names, or CLIP_HOST for the
 * host's text, the host's clipboard as it is now where the host has one.
 * Then how many code page bytes the source holds, and byte i of them, a
 * line end as CR. clip_paste_end says the text has all been read.
 * clip_paste_break says a Ctrl-C drops the pastes: the host's text that
 * waits for one is let go, and it is true when what is being typed is a
 * paste, which ends, and false when it is plain text, which goes on.
 * clip_paste_drop says everything queued goes: all the host's text is let
 * go. */
uint8_t clip_paste_source(uint8_t source);
size_t clip_size(uint8_t source);
char clip_at(uint8_t source, size_t i);
void clip_paste_end(void);
bool clip_paste_break(void);
void clip_paste_drop(void);

/* A failed load can leave a buffer longer than it holds; both are emptied. */
void clip_reset(void);
extern const sst_table_t clip_sst;

#define CLIP_DRIVER DRIVER(      \
    RESET(clip_reset)            \
    SST("CLIP", clip))

#endif /* _CORE_HID_CLIP_H_ */
