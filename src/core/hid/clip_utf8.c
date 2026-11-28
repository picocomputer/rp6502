/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The buffers (clip.h) for a machine whose host has a clipboard of its own,
 * in UTF-8: the primary here, as cell_text words, so a word keeps a
 * character's code and set and a line end is CLIP_LINE, which no character
 * is; and the clipboard the host's, given each copy as UTF-8, which keeps
 * the character each glyph shows (cell_text_unicode). A paste of the
 * clipboard types the host's text (paste_host.h). */

#include "core/hid/clip.h"
#include "core/hid/paste_host.h"
#include "core/str/unicode.h"
#include "core/sys/sst.h"
#include "core/term/cell.h"
#include "host/host.h"
#include "machine.h"
#include <string.h>

#define CLIP_LINE 0x8000u
#define CLIP_CHAR (CELL_TEXT_CODE | CELL_TEXT_SET)
_Static_assert(!(CLIP_LINE & CLIP_CHAR), "a line end is no character");

static uint16_t clip_primary[CLIP_SIZE];
static uint16_t clip_primary_len;

/* A copy for the host's clipboard, as UTF-8: each character at most three
 * bytes, as every one cell_text_unicode gives is in the BMP. */
static char clip_out[CLIP_SIZE * 3 + 1];

static bool clip_sst_valid(sst_cursor_t *c)
{
    if (clip_primary_len > CLIP_SIZE)
        return sst_refuse(c, "a buffer holding more than it can");
    return true;
}

#define CLIP_SST(X)                                             \
    X(U16S, clip_primary)                                       \
    X(U16, clip_primary_len)                                    \
    X(VALIDATE, clip_sst_valid)                                 \
    X(SKIP, clip_out, "the host's text, which no save carries")
SST_TABLE(clip, CLIP_SST)

void clip_reset(void)
{
    clip_primary_len = 0;
}

typedef struct
{
    screen_sink_t sink;
} clip_sink_t;

static void clip_put(uint16_t word)
{
    if (clip_primary_len < CLIP_SIZE)
        clip_primary[clip_primary_len++] = word;
}

static void clip_text(screen_sink_t *k, uint16_t word)
{
    (void)k;
    clip_put(word & CLIP_CHAR);
}

static void clip_line(screen_sink_t *k)
{
    (void)k;
    clip_put(CLIP_LINE);
}

typedef struct
{
    screen_sink_t sink;
    size_t len;
} clip_out_sink_t;

static void clip_out_text(screen_sink_t *k, uint16_t word)
{
    clip_out_sink_t *u = (clip_out_sink_t *)k;
    if (u->len + 3 < sizeof clip_out)
        u->len += (size_t)unicode_utf8_put(cell_text_unicode(word), &clip_out[u->len]);
}

static void clip_out_line(screen_sink_t *k)
{
    clip_out_sink_t *u = (clip_out_sink_t *)k;
    if (u->len + 1 < sizeof clip_out)
        clip_out[u->len++] = '\n';
}

#define CLIP_OUT_SINK {{clip_out_text, clip_out_line}, 0}

static void clip_out_send(const clip_out_sink_t *u)
{
    clip_out[u->len] = 0;
    host_clipboard_set(clip_out);
}

void clip_copy(uint8_t which, screen_t *s, uint8_t y0, uint8_t x0, uint8_t y1, uint8_t x1)
{
    if (which == CLIP_CLIPBOARD)
    {
        clip_out_sink_t u = CLIP_OUT_SINK;
        screen_copy(s, y0, x0, y1, x1, &u.sink);
        clip_out_send(&u);
        return;
    }
    typed_clip_changed(which);
    clip_primary_len = 0;
    clip_sink_t k = {{clip_text, clip_line}};
    screen_copy(s, y0, x0, y1, x1, &k.sink);
}

/* A code page byte is the cell_text word of the code page's set. */
_Static_assert(CELL_SET_PAGE == 0, "a code page byte is its own word");
void HOST_IN_FLASH_FUNC clip_copy_bytes(uint8_t which, const char *bytes, size_t n)
{
    if (n > CLIP_SIZE)
        n = CLIP_SIZE;
    if (which == CLIP_CLIPBOARD)
    {
        clip_out_sink_t u = CLIP_OUT_SINK;
        for (size_t i = 0; i < n; i++)
            u.sink.text(&u.sink, (uint8_t)bytes[i]);
        clip_out_send(&u);
        return;
    }
    typed_clip_changed(which);
    for (size_t i = 0; i < n; i++)
        clip_primary[i] = (uint8_t)bytes[i];
    clip_primary_len = (uint16_t)n;
}

/* A paste of the clipboard types the host's, as it is when the paste's
 * turn comes, and a paste of text the host handed over types that. Either
 * is the host's text, kept until it is typed (paste_host.h). */
uint8_t clip_paste_source(uint8_t source)
{
    if (source == CLIP_PRIMARY)
        return source;
    paste_host_begin(source == CLIP_CLIPBOARD ? host_clipboard_get() : NULL);
    return CLIP_HOST;
}

size_t clip_size(uint8_t source)
{
    return source == CLIP_HOST ? paste_host_size() : clip_primary_len;
}

char clip_at(uint8_t source, size_t i)
{
    if (source == CLIP_HOST)
        return paste_host_at(i);
    const uint16_t e = clip_primary[i];
    return e == CLIP_LINE ? '\r' : (char)cell_text_byte(e);
}

void clip_paste_end(void)
{
    paste_host_end();
}

bool clip_paste_break(void)
{
    return paste_host_break();
}

void clip_paste_drop(void)
{
    paste_host_drop();
}
