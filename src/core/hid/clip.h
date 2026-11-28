/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The two buffers of copied text, as Linux has them: the primary, which
 * selecting fills and the middle button pastes, and the clipboard, which
 * the copy keys fill and the paste keys paste. The primary is the
 * console's, kept here. The clipboard is the host's (host/host.h).
 *
 * A buffer holds the text of the largest screen, 80 columns by
 * TERM_MAX_HEIGHT rows with a line end after each row. The machine builds
 * one of two files, by what its host's clipboard is:
 * - clip_utf8.c, for a host with a clipboard of its own (a desktop's, a web
 *   page's): it keeps the primary as cell_text words, and gives the host
 *   each copy in UTF-8, keeping the character each glyph shows.
 * - clip_codepage.c, for a host with none (the Pico, the Pocket, libretro):
 *   it keeps both as code page bytes (cell_text_byte), the clipboard in the
 *   host's buffer, and saves both with the machine.
 *
 * A paste is an entry in the keyboard's queue (typed.h), which reads its
 * buffer when its turn comes, where the text is. A window's paste key is
 * no such entry: the window reads its clipboard as the key goes down and
 * hands the text over in the key's place (event_paste). */

#ifndef _CORE_HID_CLIP_H_
#define _CORE_HID_CLIP_H_

#include "core/hid/typed.h"
#include "core/sys/driver.h"
#include "core/sys/sst.h"
#include "core/term/screen.h"
#include "machine.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* CLIP_HOST is no buffer: it is the host's text a paste types, on a
 * machine that builds clip_utf8.c (paste_host.h). */
enum
{
    CLIP_PRIMARY,
    CLIP_CLIPBOARD,
    CLIP_COUNT,
    CLIP_HOST = CLIP_COUNT,
};

#define CLIP_SIZE ((80 + 1) * TERM_MAX_HEIGHT)

#if CLIP_CODEPAGE
/* The clipboard of a host with no clipboard of its own (machine.h), which
 * builds clip_codepage.c: its code page bytes, the machine's to fill, read
 * and save, kept in the host's clip.c. */
extern uint8_t host_clipboard_bytes[CLIP_SIZE];
extern uint16_t host_clipboard_len;
#endif

/* The text of screen s from row y0, column x0 up to row y1, column x1, as
 * screen_copy takes it, into buffer which. A paste being typed from the
 * buffer ends here. */
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
