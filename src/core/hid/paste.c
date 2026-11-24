/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/clip.h"
#include "core/hid/paste.h"
#include "core/hid/typed.h"
#include "core/com/rln.h"
#include "core/sys/ria.h"
#include "core/sys/ring.h"
#include "core/sys/sst.h"
#include "core/term/vterm.h"
#include "machine.h"
#include <string.h>

/* In RAM, as paste_read and paste_peek read them. */
static const char HOST_IN_RAM paste_open_bracket[] = "\33[200~";
static const char HOST_IN_RAM paste_close_bracket[] = "\33[201~";
#define PASTE_BRACKET (sizeof paste_open_bracket - 1)

/* The paste's text staged as code page bytes, read before the typed queue's
 * bytes behind it. The stage holds one paste's text at a time. */
static RING(char, TYPED_QUEUE_SIZE) paste_stage;

/* The typed queue's first paste_ahead bytes were there when the paste
 * started, and are read before it. */
static uint32_t paste_ahead;

/* The mark: a place in the row that nothing is read past until the line
 * editor has taken what it holds there (paste.h). It is among the typed
 * queue's bytes, paste_mark of them still to read before it. Set with a
 * paste going in, it is behind that paste too. A paste begun while it is
 * set is behind it, and it is ahead of that paste, which waits for it. */
enum
{
    PASTE_MARK_NONE,
    PASTE_MARK_AHEAD,
    PASTE_MARK_BEHIND,
};
static uint8_t paste_mark_at;
static uint16_t paste_mark;

/* The brackets round the stage: the bytes of the opening bracket still to
 * be read before it, and of the closing bracket after it, which counts only
 * once the paste has no more text. */
static uint8_t paste_open;
static uint8_t paste_close;

/* Where the text comes from while there is more: the host through
 * paste_host_fill, or a machine buffer, PASTE_FROM_CLIP plus its clip.h
 * number, from its element paste_at. */
enum
{
    PASTE_FROM_NONE,
    PASTE_FROM_HOST,
    PASTE_FROM_CLIP,
};
static uint8_t paste_from;
static uint16_t paste_at;
static paste_fill_t paste_host_fill;

/* The paste from paste_from has given nothing yet: it waits until the one
 * before it has all been read, and starts, its opening bracket first, with
 * the first text it gives. */
static bool paste_waiting;

/* The closing bracket follows the text, and for a paste still waiting, the
 * opening bracket comes before it. */
static bool paste_bracketed;

/* The stage holds the host's text, which no save carries. */
static bool paste_hosted;

/* Nothing of a paste is left to read: no bracket and no staged text. */
static bool HOST_IN_RAM_FUNC paste_drained(void)
{
    return !paste_open && !RING_LEN(&paste_stage) && !paste_close;
}

bool HOST_IN_RAM_FUNC paste_staging(void)
{
    return paste_from != PASTE_FROM_NONE;
}

bool HOST_IN_RAM_FUNC paste_busy(void)
{
    return !paste_drained() || paste_staging();
}

/* Up to room of the *left bytes still to read of bracket. */
static size_t HOST_IN_RAM_FUNC paste_bracket_read(const char *bracket, uint8_t *left,
                                                  char *dst, size_t room)
{
    const size_t n = *left < room ? *left : room;
    memcpy(dst, bracket + PASTE_BRACKET - *left, n);
    *left = (uint8_t)(*left - n);
    return n;
}

void HOST_IN_FLASH_FUNC paste_mark_set(void)
{
    paste_mark = (uint16_t)(TYPED_QUEUE_SIZE - typed_free());
    paste_mark_at = paste_busy() ? PASTE_MARK_BEHIND : PASTE_MARK_AHEAD;
}

void HOST_IN_FLASH_FUNC paste_unmark(void)
{
    paste_mark_at = PASTE_MARK_NONE;
}

/* A paste waits behind the mark, with nothing of the one before it left
 * to read: the typed queue's bytes before the mark are next. */
static bool HOST_IN_RAM_FUNC paste_waits_for_mark(void)
{
    return paste_waiting && paste_mark_at == PASTE_MARK_AHEAD && paste_drained();
}

bool HOST_IN_RAM_FUNC paste_mark_reached(void)
{
    return paste_mark_at && !paste_mark &&
           (paste_mark_at == PASTE_MARK_AHEAD ? paste_drained() : !paste_busy());
}

/* The typed queue's bytes, as far as the mark. */
static size_t HOST_IN_RAM_FUNC paste_typed_read(char *buf, size_t length)
{
    if (!paste_mark_at)
        return typed_read(buf, length);
    const size_t got = typed_read(buf, length < paste_mark ? length : paste_mark);
    paste_mark = (uint16_t)(paste_mark - got);
    return got;
}

static int HOST_IN_RAM_FUNC paste_typed_peek(size_t offset)
{
    return paste_mark_at && offset >= paste_mark ? -1 : typed_peek(offset);
}

size_t HOST_IN_RAM_FUNC paste_read(char *buf, size_t length)
{
    size_t got = 0;
    if (paste_ahead)
    {
        got = paste_typed_read(buf, length < paste_ahead ? length : paste_ahead);
        paste_ahead -= (uint32_t)got;
        if (paste_ahead)
            return got;
    }
    got += paste_bracket_read(paste_open_bracket, &paste_open, buf + got, length - got);
    got += RING_OUT(&paste_stage, buf + got, (uint32_t)(length - got));
    if (!RING_LEN(&paste_stage))
        got += paste_bracket_read(paste_close_bracket, &paste_close, buf + got, length - got);
    if (paste_busy() && !paste_waits_for_mark())
        return got;
    return got + paste_typed_read(buf + got, length - got);
}

/* Past the stage while more is to come, nothing has arrived yet. */
int HOST_IN_RAM_FUNC paste_peek(size_t offset)
{
    if (offset < paste_ahead)
        return paste_typed_peek(offset);
    offset -= paste_ahead;
    if (offset < paste_open)
        return (unsigned char)paste_open_bracket[PASTE_BRACKET - paste_open + offset];
    offset -= paste_open;
    const uint32_t staged = RING_LEN(&paste_stage);
    if (offset < staged)
        return (unsigned char)*RING_AT(&paste_stage, offset);
    offset -= staged;
    if (offset < paste_close)
        return (unsigned char)paste_close_bracket[PASTE_BRACKET - paste_close + offset];
    if (paste_staging() && !paste_waits_for_mark())
        return -1;
    return paste_typed_peek(paste_ahead + offset - paste_close);
}

/* The paste going in has no more text: the closing bracket comes after
 * what is staged. */
static void HOST_IN_RAM_FUNC paste_end(void)
{
    paste_from = PASTE_FROM_NONE;
    paste_close = paste_bracketed ? PASTE_BRACKET : 0;
    paste_bracketed = false;
}

void HOST_IN_RAM_FUNC paste_cut(void)
{
    if (paste_waiting)
    {
        paste_from = PASTE_FROM_NONE;
        paste_waiting = paste_bracketed = false;
    }
    else if (paste_from != PASTE_FROM_NONE)
        paste_end();
}

void paste_cancel(void)
{
    RING_RESET(&paste_stage);
    paste_ahead = 0;
    /* A mark is reached: every caller empties the typed queue too, or
     * loads over it. */
    paste_mark = 0;
    if (paste_mark_at)
        paste_mark_at = PASTE_MARK_AHEAD;
    paste_open = paste_close = 0;
    paste_from = PASTE_FROM_NONE;
    paste_waiting = paste_bracketed = paste_hosted = false;
}

void paste_clear(void)
{
    paste_cancel();
    typed_clear();
}

/* A paste going in is cut short, and this one follows it. One that starts
 * with nothing going in waits behind what the typed queue holds. */
static void paste_begin(uint8_t from, bool bracketed)
{
    paste_cut();
    if (!paste_busy())
        paste_ahead = (uint32_t)(TYPED_QUEUE_SIZE - typed_free());
    if (paste_mark_at)
        paste_mark_at = PASTE_MARK_AHEAD;
    paste_from = from;
    paste_waiting = true;
    paste_bracketed = bracketed;
}

void paste_host(paste_fill_t fill, bool bracketed)
{
    paste_host_fill = fill;
    paste_begin(PASTE_FROM_HOST, bracketed);
}

void paste_host_cut(paste_fill_t fill)
{
    if (paste_from == PASTE_FROM_HOST && paste_host_fill == fill)
        paste_cut();
}

void paste_clip(uint8_t which)
{
    paste_at = 0;
    paste_begin((uint8_t)(PASTE_FROM_CLIP + which), vterm.bracketed_paste);
}

void paste_clip_changed(uint8_t which)
{
    /* A paste going in ends here. One that waits behind a mark was begun
     * after what is held there, a cut it may be that changes the buffer
     * now, and is a paste of the buffer as changed. */
    if (paste_from == PASTE_FROM_CLIP + which &&
        !(paste_waiting && paste_mark_at == PASTE_MARK_AHEAD))
        paste_cut();
}

/* A paste waiting starts once the one before it has all been read, and
 * one that waits behind a mark, once the mark is gone: what the line
 * editor holds there may be a cut, which fills the clipboard this would
 * read. Its text is staged as far as the stage goes. Its opening bracket comes
 * with its first text, so a paste with none, of an empty buffer or of host
 * text that was all control characters, sends nothing. Text typed as
 * written may hold a Ctrl-C, which raises the SIGINT as it is staged, as a
 * typed one does. */
void paste_task(void)
{
    if (paste_waiting && (!paste_drained() || paste_mark_at == PASTE_MARK_AHEAD))
        return;
    while (paste_from != PASTE_FROM_NONE)
    {
        const size_t room = RING_AVAIL(&paste_stage);
        if (!room)
            return;
        char text[TYPED_QUEUE_SIZE];
        const size_t n = paste_from == PASTE_FROM_HOST
                             ? paste_host_fill(text, room)
                             : clip_fill((uint8_t)(paste_from - PASTE_FROM_CLIP), &paste_at,
                                         text, room);
        if (!n)
        {
            paste_cut();
            return;
        }
        if (paste_waiting)
        {
            paste_waiting = false;
            paste_open = paste_bracketed ? PASTE_BRACKET : 0;
            paste_hosted = paste_from == PASTE_FROM_HOST;
        }
        if (memchr(text, 0x03, n))
            ria_trigger_sigint();
        RING_IN(&paste_stage, text, (uint32_t)n);
    }
}

/* The host's text is never saved: not the text staged, nor a host paste
 * still to come. What the program has been given of a paste of it is
 * machine state, though: one whose opening bracket the program has begun
 * to read is saved as that paste cut short, its brackets with no text
 * between, so the closing bracket still comes. One the program has seen
 * nothing of goes. A paste of a machine buffer is saved whole: its text,
 * its brackets and where it is in the buffer. */
static void paste_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    const bool seen = !paste_hosted || paste_open < PASTE_BRACKET;
    const bool host = paste_from == PASTE_FROM_HOST;
    const uint32_t len = paste_hosted ? 0 : RING_LEN(&paste_stage);
    uint8_t close = seen ? paste_close : 0;
    if (host && !paste_waiting && seen && paste_bracketed)
        close = PASTE_BRACKET;
    sst_put_u32(c, len);
    for (uint32_t i = 0; i < TYPED_QUEUE_SIZE; i++)
        sst_put_u8(c, i < len ? (uint8_t)*RING_AT(&paste_stage, i) : 0);
    sst_put_u32(c, paste_ahead);
    sst_put_u8(c, seen ? paste_open : 0);
    sst_put_u8(c, close);
    sst_put_u8(c, host ? PASTE_FROM_NONE : paste_from);
    sst_put_u16(c, host ? 0 : paste_at);
    sst_put_bool(c, !host && paste_waiting);
    sst_put_bool(c, !host && paste_bracketed);
    sst_put_u16(c, paste_mark);
    sst_put_u8(c, paste_mark_at);
}

static bool paste_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    paste_cancel();
    const uint32_t len = sst_get_u32(c);
    char bytes[TYPED_QUEUE_SIZE];
    sst_get(c, bytes, sizeof bytes);
    const uint32_t ahead = sst_get_u32(c);
    const uint8_t open = sst_get_u8(c);
    const uint8_t close = sst_get_u8(c);
    const uint8_t from = sst_get_u8(c);
    const uint16_t at = sst_get_u16(c);
    const bool waiting = sst_get_bool(c);
    const bool bracketed = sst_get_bool(c);
    const uint16_t mark = sst_get_u16(c);
    const uint8_t mark_at = sst_get_u8(c);
    if (!sst_ok(c))
        return false;
    if (len > TYPED_QUEUE_SIZE || ahead > TYPED_QUEUE_SIZE - typed_free() ||
        open > PASTE_BRACKET || close > PASTE_BRACKET)
        return sst_refuse(c, "a paste holding more than it can");
    if ((from != PASTE_FROM_NONE && (from < PASTE_FROM_CLIP || from >= PASTE_FROM_CLIP + CLIP_COUNT)) ||
        at > CLIP_SIZE || (from == PASTE_FROM_NONE && (waiting || bracketed)) ||
        (close && from != PASTE_FROM_NONE && !waiting))
        return sst_refuse(c, "a paste from nowhere");
    /* The line editor, loaded before this, holds something at a mark. */
    if (mark_at > PASTE_MARK_BEHIND || mark > TYPED_QUEUE_SIZE - typed_free() ||
        (mark_at != PASTE_MARK_NONE) != rln_holds() ||
        (mark_at == PASTE_MARK_AHEAD && from != PASTE_FROM_NONE && !waiting))
        return sst_refuse(c, "a mark with nothing held at it");
    RING_IN(&paste_stage, bytes, len);
    paste_mark = mark;
    paste_mark_at = mark_at;
    paste_ahead = ahead;
    paste_open = open;
    paste_close = close;
    paste_from = from;
    paste_at = at;
    paste_waiting = waiting;
    paste_bracketed = bracketed;
    return true;
}

#define PASTE_SST(X)                                                   \
    X(FN, paste_sst_save, paste_sst_load,                              \
      4 + TYPED_QUEUE_SIZE + 4 + 1 + 1 + 1 + 2 + 1 + 1 + 2 + 1, 2)     \
    X(IN_FN, paste_stage)                                              \
    X(IN_FN, paste_ahead)                                              \
    X(IN_FN, paste_open)                                               \
    X(IN_FN, paste_close)                                              \
    X(IN_FN, paste_from)                                               \
    X(IN_FN, paste_at)                                                 \
    X(IN_FN, paste_waiting)                                            \
    X(IN_FN, paste_bracketed)                                          \
    X(IN_FN, paste_mark)                                               \
    X(IN_FN, paste_mark_at)                                            \
    X(SKIP, paste_host_fill, "the host's text, which every load ends") \
    X(SKIP, paste_hosted, "the host's text, which every load ends")
SST_TABLE(paste, PASTE_SST)
