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
#include "core/hid/paste.h"
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
    return clip_host_set(clip_utf8);
}

const char *clip_host_get(void)
{
    paste_host_clipboard_changed();
    return host_clipboard_get();
}

bool clip_host_set(const char *utf8)
{
    paste_host_clipboard_changed();
    return host_clipboard_set(utf8);
}

/* The clipboard goes to the host's when it has one. */
void clip_copy(uint8_t which, screen_t *s, uint8_t y0, uint8_t x0, uint8_t y1, uint8_t x1)
{
    paste_clip_changed(which);
    if (which == CLIP_CLIPBOARD)
    {
        clip_utf8_sink_t u = CLIP_UTF8_SINK;
        screen_copy(s, y0, x0, y1, x1, &u.sink);
        if (clip_utf8_send(&u))
            return;
    }
    clip_len[which] = 0;
    clip_sink_t k = {{clip_text, clip_line}, which};
    screen_copy(s, y0, x0, y1, x1, &k.sink);
}

void HOST_IN_FLASH_FUNC clip_copy_primary(void)
{
    if (!clip_len[CLIP_PRIMARY])
        return;
    paste_clip_changed(CLIP_CLIPBOARD);
    const uint16_t *primary = &clip_buf[CLIP_PRIMARY * CLIP_SIZE];
    clip_utf8_sink_t u = CLIP_UTF8_SINK;
    for (unsigned i = 0; i < clip_len[CLIP_PRIMARY]; i++)
        if (primary[i] == CLIP_LINE)
            u.sink.line(&u.sink);
        else
            u.sink.text(&u.sink, primary[i]);
    if (clip_utf8_send(&u))
        return;
    memcpy(&clip_buf[CLIP_CLIPBOARD * CLIP_SIZE], primary,
           clip_len[CLIP_PRIMARY] * sizeof *primary);
    clip_len[CLIP_CLIPBOARD] = clip_len[CLIP_PRIMARY];
}

/* A code page byte is the cell_text word of the code page's set. */
_Static_assert(CELL_SET_PAGE == 0, "a code page byte is its own word");
void HOST_IN_FLASH_FUNC clip_copy_bytes(uint8_t which, const char *bytes, size_t n)
{
    paste_clip_changed(which);
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
    uint16_t *buf = &clip_buf[which * CLIP_SIZE];
    for (size_t i = 0; i < n; i++)
        buf[i] = (uint8_t)bytes[i];
    clip_len[which] = (uint16_t)n;
}

void HOST_IN_FLASH_FUNC clip_paste(uint8_t which)
{
    const char *host = which == CLIP_CLIPBOARD ? clip_host_get() : NULL;
    if (host)
        paste_start(host);
    else
        paste_clip(which);
}

size_t clip_fill(uint8_t which, uint16_t *at, char *dst, size_t room)
{
    size_t n = 0;
    while (n < room && *at < clip_len[which])
    {
        const uint16_t e = clip_buf[which * CLIP_SIZE + (*at)++];
        dst[n++] = e == CLIP_LINE ? '\r' : (char)cell_text_byte(e);
    }
    return n;
}
