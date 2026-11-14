/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/paste.h"
#include "core/str/oem.h"
#include "machine.h"
#include <stdlib.h>
#include <string.h>

/* Text still being delivered to the keyboard ring, NULL when idle. */
static char *paste_buf;
static size_t paste_len, paste_pos;

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

void paste_start(const char *utf8)
{
    paste_cancel();
    size_t n = utf8 ? strlen(utf8) : 0;
    if (!n)
        return;
    paste_buf = malloc(n);
    if (!paste_buf)
        return;
    memcpy(paste_buf, utf8, n);
    paste_len = n;
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
