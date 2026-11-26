/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The buffers (clip.h) as cell_text words, for a machine whose host may
 * have a clipboard, so a copy to it keeps the character each glyph shows
 * (cell_text_unicode). A word keeps a character's code and set, and a line
 * end is CLIP_LINE, which no character is. */

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

static uint16_t clip_buf[CLIP_COUNT * CLIP_SIZE];
static uint16_t clip_len[CLIP_COUNT];

/* A copy for the host's clipboard, as UTF-8: each character at most three
 * bytes, as every one cell_text_unicode gives is in the BMP. */
static char clip_utf8[CLIP_SIZE * 3 + 1];

static bool clip_sst_valid(sst_cursor_t *c)
{
    for (unsigned i = 0; i < CLIP_COUNT; i++)
        if (clip_len[i] > CLIP_SIZE)
            return sst_refuse(c, "a buffer holding more than it can");
    return true;
}

#define CLIP_SST(X)                                              \
    X(U16S, clip_buf)                                            \
    X(U16S, clip_len)                                            \
    X(VALIDATE, clip_sst_valid)                                  \
    X(SKIP, clip_utf8, "the host's text, which no save carries")
SST_TABLE(clip, CLIP_SST)

void clip_reset(void)
{
    memset(clip_len, 0, sizeof clip_len);
}

typedef struct
{
    screen_sink_t sink;
    uint8_t which;
} clip_sink_t;

static void clip_put(screen_sink_t *k, uint16_t word)
{
    const uint8_t which = ((clip_sink_t *)k)->which;
    if (clip_len[which] < CLIP_SIZE)
        clip_buf[which * CLIP_SIZE + clip_len[which]++] = word;
}

static void clip_text(screen_sink_t *k, uint16_t word)
{
    clip_put(k, word & CLIP_CHAR);
}

static void clip_line(screen_sink_t *k)
{
    clip_put(k, CLIP_LINE);
}

typedef struct
{
    screen_sink_t sink;
    size_t len;
} clip_utf8_sink_t;

static void clip_utf8_text(screen_sink_t *k, uint16_t word)
{
    clip_utf8_sink_t *u = (clip_utf8_sink_t *)k;
    if (u->len + 3 < sizeof clip_utf8)
        u->len += (size_t)unicode_utf8_put(cell_text_unicode(word), &clip_utf8[u->len]);
}

static void clip_utf8_line(screen_sink_t *k)
{
    clip_utf8_sink_t *u = (clip_utf8_sink_t *)k;
    if (u->len + 1 < sizeof clip_utf8)
        clip_utf8[u->len++] = '\n';
}

#define CLIP_UTF8_SINK {{clip_utf8_text, clip_utf8_line}, 0}

/* What the sink took goes to the host's clipboard: false when it has none. */
static bool clip_utf8_send(const clip_utf8_sink_t *u)
{
    clip_utf8[u->len] = 0;
    return host_clipboard_set(clip_utf8);
}

/* The clipboard goes to the host's when it has one, and the machine's
 * buffer stays as it was. */
void clip_copy(uint8_t which, screen_t *s, uint8_t y0, uint8_t x0, uint8_t y1, uint8_t x1)
{
    if (which == CLIP_CLIPBOARD)
    {
        clip_utf8_sink_t u = CLIP_UTF8_SINK;
        screen_copy(s, y0, x0, y1, x1, &u.sink);
        if (clip_utf8_send(&u))
            return;
    }
    typed_clip_changed(which);
    clip_len[which] = 0;
    clip_sink_t k = {{clip_text, clip_line}, which};
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
        clip_utf8_sink_t u = CLIP_UTF8_SINK;
        for (size_t i = 0; i < n; i++)
            u.sink.text(&u.sink, (uint8_t)bytes[i]);
        if (clip_utf8_send(&u))
            return;
    }
    typed_clip_changed(which);
    uint16_t *buf = &clip_buf[which * CLIP_SIZE];
    for (size_t i = 0; i < n; i++)
        buf[i] = (uint8_t)bytes[i];
    clip_len[which] = (uint16_t)n;
}

/* A paste of the clipboard types the host's where it has one, asked for
 * as the paste's turn comes, perhaps more than once if the host gives it
 * later, and a paste of text the host handed over types that. Either is the host's text, kept until it is typed
 * (paste_host.h). */
uint8_t clip_paste_source(uint8_t source)
{
    const char *host = source == CLIP_CLIPBOARD ? host_clipboard_get() : NULL;
    if (host == HOST_CLIPBOARD_LATER)
        return CLIP_LATER;
    if (!host && source != CLIP_HOST)
        return source;
    paste_host_begin(host);
    return CLIP_HOST;
}

size_t clip_size(uint8_t source)
{
    return source == CLIP_HOST ? paste_host_size() : clip_len[source];
}

char clip_at(uint8_t source, size_t i)
{
    if (source == CLIP_HOST)
        return paste_host_at(i);
    const uint16_t e = clip_buf[source * CLIP_SIZE + i];
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
