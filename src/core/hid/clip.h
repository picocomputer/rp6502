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
 * never the host's text. */

#ifndef _CORE_HID_CLIP_H_
#define _CORE_HID_CLIP_H_

#include "core/sys/driver.h"
#include "core/sys/sst.h"
#include "core/term/screen.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum
{
    CLIP_PRIMARY,
    CLIP_CLIPBOARD,
    CLIP_COUNT,
};

#define CLIP_SIZE ((80 + 1) * TERM_MAX_HEIGHT)

/* The text of screen s from row y0, column x0 up to row y1, column x1, as
 * screen_copy takes it, into buffer which. A paste from the buffer ends
 * here. */
void clip_copy(uint8_t which, screen_t *s, uint8_t y0, uint8_t x0, uint8_t y1, uint8_t x1);

/* The line editor's text, n code page bytes as it holds them, goes into
 * buffer which: the clipboard as UTF-8 to the host's where it has one. A
 * paste from the buffer ends here. */
void clip_copy_bytes(uint8_t which, const char *bytes, size_t n);

/* The copy keys: the primary goes to the clipboard, as UTF-8 to the host's
 * where it has one. The primary has no cut, so it keeps its text. A paste
 * from the clipboard ends here. An empty primary copies nothing, and the
 * clipboard keeps what it held. */
void clip_copy_primary(void);

/* Buffer which is pasted (paste.h), the host's clipboard where it has one. */
void clip_paste(uint8_t which);

/* The host's clipboard (host.h), on a machine that builds clip_text.c:
 * everything that asks for it or sets it goes through these, which first
 * end a paste of the text it gave, as either can write over that text. */
const char *clip_host_get(void);
bool clip_host_set(const char *utf8);

/* The machine buffer which as code page bytes, for the paste stage: up to
 * room of them from element *at on, a line end as CR, and *at moves past
 * them. None at the buffer's end. */
size_t clip_fill(uint8_t which, uint16_t *at, char *dst, size_t room);

/* A failed load can leave a buffer longer than it holds; both are emptied. */
void clip_reset(void);
extern const sst_table_t clip_sst;

#define CLIP_DRIVER DRIVER(      \
    RESET(clip_reset)            \
    SST("CLIP", clip))

#endif /* _CORE_HID_CLIP_H_ */
