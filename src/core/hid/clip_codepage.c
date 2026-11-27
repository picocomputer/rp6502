/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The buffers (clip.h) as code page bytes, for a machine with no system
 * clipboard: the primary here, and the clipboard the host keeps
 * (host_clipboard_bytes), both saved with the machine in this chunk. Every
 * byte is a character's, never a control byte (cell_text_byte), so a line
 * end is kept as CR, as it is pasted. */

#include "core/hid/clip.h"
#include "core/sys/sst.h"
#include "core/term/cell.h"
#include "host/host.h"
#include "machine.h"
#include <string.h>

#if !CLIP_CODEPAGE
#error "a machine that builds clip_codepage.c says so (CLIP_CODEPAGE)"
#endif

static uint8_t clip_primary[CLIP_SIZE];
static uint16_t clip_primary_len;

static bool clip_sst_valid(sst_cursor_t *c)
{
    if (clip_primary_len > CLIP_SIZE || host_clipboard_len > CLIP_SIZE)
        return sst_refuse(c, "a buffer holding more than it can");
    return true;
}

#define CLIP_SST(X)                 \
    X(BYTES, clip_primary)          \
    X(U16, clip_primary_len)        \
    X(BYTES, host_clipboard_bytes)  \
    X(U16, host_clipboard_len)      \
    X(VALIDATE, clip_sst_valid)
SST_TABLE(clip, CLIP_SST)

void clip_reset(void)
{
    clip_primary_len = host_clipboard_len = 0;
}

/* Buffer which's bytes and its length. They are read from RAM, as the
 * keyboard's row is. */
static uint8_t *HOST_IN_RAM_FUNC clip_bytes(uint8_t which)
{
    return which == CLIP_PRIMARY ? clip_primary : host_clipboard_bytes;
}

static uint16_t *HOST_IN_RAM_FUNC clip_len(uint8_t which)
{
    return which == CLIP_PRIMARY ? &clip_primary_len : &host_clipboard_len;
}

typedef struct
{
    screen_sink_t sink;
    uint8_t which;
} clip_sink_t;

static void clip_put(screen_sink_t *k, uint8_t b)
{
    const uint8_t which = ((clip_sink_t *)k)->which;
    uint16_t *len = clip_len(which);
    if (*len < CLIP_SIZE)
        clip_bytes(which)[(*len)++] = b;
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
    typed_clip_changed(which);
    *clip_len(which) = 0;
    clip_sink_t k = {{clip_text, clip_line}, which};
    screen_copy(s, y0, x0, y1, x1, &k.sink);
}

void HOST_IN_FLASH_FUNC clip_copy_bytes(uint8_t which, const char *bytes, size_t n)
{
    typed_clip_changed(which);
    if (n > CLIP_SIZE)
        n = CLIP_SIZE;
    memcpy(clip_bytes(which), bytes, n);
    *clip_len(which) = (uint16_t)n;
}

/* A paste reads its buffer where it is. There is no host's text here to
 * let go. */
uint8_t HOST_IN_RAM_FUNC clip_paste_source(uint8_t source)
{
    return source;
}

size_t HOST_IN_RAM_FUNC clip_size(uint8_t source)
{
    return *clip_len(source);
}

char HOST_IN_RAM_FUNC clip_at(uint8_t source, size_t i)
{
    return (char)clip_bytes(source)[i];
}

void HOST_IN_RAM_FUNC clip_paste_end(void)
{
}

bool clip_paste_break(void)
{
    return true;
}

void clip_paste_drop(void)
{
}
