/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/paste.h"
#include "core/str/oem.h"
#include "core/sys/sst.h"
#include "core/term/cells.h"
#include "machine.h"
#include <stdlib.h>
#include <string.h>

/* Text still being delivered to the keyboard ring, NULL when idle. */
static char *paste_buf;
static size_t paste_len, paste_pos;

#define PASTE_SST(X)                                        \
    X(SKIP, paste_buf, "every load cancels the paste")      \
    X(SKIP, paste_len)                                      \
    X(SKIP, paste_pos)
SST_TABLE(paste, PASTE_SST)

void paste_cancel(void)
{
    free(paste_buf);
    paste_buf = NULL;
}

void paste_clear(void)
{
    paste_cancel();
    manifold_keyboard_clear();
}

/* A program that set bracketed paste (?2004) gets the paste between
 * ESC [ 200 ~ and ESC [ 201 ~, with any ESC of the text left out, so the
 * text cannot end the bracket early. */
static const char paste_open[] = "\33[200~";
static const char paste_close[] = "\33[201~";

void paste_start(const char *utf8)
{
    paste_cancel();
    size_t n = utf8 ? strlen(utf8) : 0;
    if (!n)
        return;
    bool bracket = cells_term.bracketed_paste;
    paste_buf = malloc(n + (bracket ? sizeof paste_open + sizeof paste_close : 0));
    if (!paste_buf)
        return;
    size_t len = 0;
    if (bracket)
    {
        memcpy(paste_buf, paste_open, sizeof paste_open - 1);
        len = sizeof paste_open - 1;
        for (size_t i = 0; i < n; i++)
            if (utf8[i] != '\33')
                paste_buf[len++] = utf8[i];
        memcpy(paste_buf + len, paste_close, sizeof paste_close - 1);
        len += sizeof paste_close - 1;
    }
    else
    {
        memcpy(paste_buf, utf8, n);
        len = n;
    }
    paste_len = len;
    paste_pos = 0;
}

bool paste_busy(void)
{
    return paste_buf != NULL;
}

void paste_task(void)
{
    if (!paste_buf)
        return;
    /* A quarter of the ring is left free so that live typing still fits
     * during a long paste. */
    while (paste_pos < paste_len && manifold_keyboard_free() > MANIFOLD_RING_SIZE / 4)
    {
        char out[16];
        size_t room = manifold_keyboard_free() - MANIFOLD_RING_SIZE / 4;
        if (room > sizeof out)
            room = sizeof out;
        size_t taken = 0;
        size_t n = oem_from_utf8_run(paste_buf + paste_pos,
                                     paste_len - paste_pos,
                                     true, out, room, &taken);
        if (!taken)
            break;
        manifold_keyboard_push(out, n);
        paste_pos += taken;
    }
    if (paste_pos >= paste_len)
        paste_cancel();
}
