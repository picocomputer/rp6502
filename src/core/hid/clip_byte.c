/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The buffers (clip.h) as code page bytes, for a machine whose host has no
 * clipboard. Every byte is a character's, never a control byte
 * (cell_text_byte), so a line end is kept as CR, as it is pasted. */

#include "core/hid/clip.h"
#include "core/hid/paste.h"
#include "core/sys/sst.h"
#include "core/term/cell.h"
#include <string.h>

static uint8_t clip_buf[CLIP_COUNT * CLIP_SIZE];
static uint16_t clip_len[CLIP_COUNT];

static bool clip_sst_valid(sst_cursor_t *c)
{
    for (unsigned i = 0; i < CLIP_COUNT; i++)
        if (clip_len[i] > CLIP_SIZE)
            return sst_refuse(c, "a buffer holding more than it can");
    return true;
}

#define CLIP_SST(X)        \
    X(BYTES, clip_buf)     \
    X(U16S, clip_len)      \
    X(VALIDATE, clip_sst_valid)
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

static void clip_put(screen_sink_t *k, uint8_t b)
{
    const uint8_t which = ((clip_sink_t *)k)->which;
    if (clip_len[which] < CLIP_SIZE)
        clip_buf[which * CLIP_SIZE + clip_len[which]++] = b;
}

static void clip_text(screen_sink_t *k, uint16_t word)
{
    clip_put(k, cell_text_byte(word));
}

static void clip_line(screen_sink_t *k)
{
    clip_put(k, '\r');
}

void clip_copy(uint8_t which, screen_t *s, uint8_t y0, uint8_t x0, uint8_t y1, uint8_t x1)
{
    paste_clip_changed(which);
    clip_len[which] = 0;
    clip_sink_t k = {{clip_text, clip_line}, which};
    screen_copy(s, y0, x0, y1, x1, &k.sink);
}

void clip_paste(uint8_t which)
{
    paste_clip(which);
}

size_t clip_fill(uint8_t which, uint16_t *at, char *dst, size_t room)
{
    if (*at >= clip_len[which])
        return 0;
    size_t n = (size_t)(clip_len[which] - *at);
    if (n > room)
        n = room;
    memcpy(dst, &clip_buf[which * CLIP_SIZE + *at], n);
    *at = (uint16_t)(*at + n);
    return n;
}
