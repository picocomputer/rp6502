/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/paste.h"
#include "core/hid/typed.h"
#include "core/str/oem.h"
#include "core/sys/sst.h"
#include "core/term/vterm.h"
#include "machine.h"
#include <stdlib.h>
#include <string.h>

/* Text still being delivered to the typed queue, NULL when idle. */
static char *paste_buf;
static size_t paste_len, paste_pos;

void paste_cancel(void)
{
    free(paste_buf);
    paste_buf = NULL;
}

/* The text came from outside the machine and is on its way in, so a save
 * carries none of it and every load ends it. */
static void paste_sst_nothing(sst_cursor_t *c, unsigned flags)
{
    (void)c;
    (void)flags;
}

static bool paste_sst_cancel(sst_cursor_t *c, unsigned flags)
{
    (void)c;
    (void)flags;
    paste_cancel();
    return true;
}

#define PASTE_SST(X)                                        \
    X(FN, paste_sst_nothing, paste_sst_cancel, 0, 1)        \
    X(SKIP, paste_buf, "every load cancels the paste")      \
    X(SKIP, paste_len)                                      \
    X(SKIP, paste_pos)
SST_TABLE(paste, PASTE_SST)

void paste_clear(void)
{
    paste_cancel();
    typed_clear();
}

/* End the paste in progress, then make room for n bytes of text and the
 * extra bytes around them. Empty text starts nothing. */
static char *paste_alloc(size_t n, size_t extra)
{
    paste_cancel();
    paste_len = paste_pos = 0;
    if (!n)
        return NULL;
    return paste_buf = malloc(n + extra);
}

void typed_text(const char *utf8)
{
    size_t n = utf8 ? strlen(utf8) : 0;
    if (!paste_alloc(n, 0))
        return;
    memcpy(paste_buf, utf8, n);
    paste_len = n;
}

/* A program that set bracketed paste (?2004) gets the paste between
 * ESC [ 200 ~ and ESC [ 201 ~, with any ESC of the text left out, so the
 * text cannot end the bracket early. A line end is pressed as Enter, the way
 * a terminal pastes: the host's clipboard ends lines in CRLF on Windows and
 * LF elsewhere. */
static const char paste_open[] = "\33[200~";
static const char paste_close[] = "\33[201~";

void paste_start(const char *utf8)
{
    size_t n = utf8 ? strlen(utf8) : 0;
    bool bracket = vterm.bracketed_paste;
    if (!paste_alloc(n, bracket ? sizeof paste_open + sizeof paste_close : 0))
        return;
    size_t len = 0;
    if (bracket)
    {
        memcpy(paste_buf, paste_open, sizeof paste_open - 1);
        len = sizeof paste_open - 1;
    }
    for (size_t i = 0; i < n; i++)
    {
        char ch = utf8[i];
        if (ch == '\33' && bracket)
            continue;
        if (ch == '\r' && utf8[i + 1] == '\n')
            i++;
        paste_buf[len++] = ch == '\n' ? '\r' : ch;
    }
    if (bracket)
    {
        memcpy(paste_buf + len, paste_close, sizeof paste_close - 1);
        len += sizeof paste_close - 1;
    }
    paste_len = len;
}

bool paste_busy(void)
{
    return paste_buf != NULL;
}

void paste_task(void)
{
    if (!paste_buf)
        return;
    /* A quarter of the queue is left free so that live typing still fits
     * during a long paste. */
    while (paste_pos < paste_len && typed_free() > TYPED_QUEUE_SIZE / 4)
    {
        char out[16];
        size_t room = typed_free() - TYPED_QUEUE_SIZE / 4;
        if (room > sizeof out)
            room = sizeof out;
        size_t taken = 0;
        size_t n = oem_from_utf8_run(paste_buf + paste_pos,
                                     paste_len - paste_pos,
                                     true, out, room, &taken);
        if (!taken)
            break;
        typed_push(out, n);
        paste_pos += taken;
    }
    if (paste_pos >= paste_len)
        paste_cancel();
}
