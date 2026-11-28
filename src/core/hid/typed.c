/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/com/rln.h"
#include "core/hid/clip.h"
#include "core/hid/typed.h"
#include "core/sys/ria.h"
#include "core/sys/ring.h"
#include "core/sys/sst.h"
#include "core/term/vterm.h"
#include "machine.h"

static RING(typed_entry_t, TYPED_QUEUE_SIZE) typed_queue;

/* In RAM, as typed_peek reads them. */
static const char HOST_IN_RAM typed_open_bracket[] = "\33[200~";
static const char HOST_IN_RAM typed_close_bracket[] = "\33[201~";
#define TYPED_BRACKET (sizeof typed_open_bracket - 1)

/* The paste being typed, in front of the queue: the source its text is
 * read from (clip.h) plus one, or 0 once it has no more, the element of
 * the source that is next, and the bytes still to read of the opening
 * bracket before the text and of the closing bracket after it. */
static uint8_t typed_paste_from;
static uint16_t typed_paste_at;
static uint8_t typed_paste_open;
static uint8_t typed_paste_close;

/* The paste being typed has no more text. */
static void HOST_IN_RAM_FUNC typed_paste_end(void)
{
    typed_paste_from = 0;
    clip_paste_end();
}

/* What is at the front becomes bytes, or stays for the line editor. An
 * entry taken back goes. A paste starts: its text is its source's as it is
 * now, bracketed as the console is set now, and a source with no text
 * starts nothing. A paste whose host has yet to give its text waits at the
 * front, and so does everything behind it. An entry of the line editor's waits there while a line
 * is being read, and goes when none is. */
static void HOST_IN_RAM_FUNC typed_front(void)
{
    while (!typed_paste_open && !typed_paste_from && !typed_paste_close &&
           RING_LEN(&typed_queue))
    {
        const typed_entry_t e = *RING_AT(&typed_queue, 0);
        if (TYPED_KIND(e) == TYPED_BYTE || (TYPED_KIND(e) > TYPED_PASTE && rln_reading()))
            return;
        if (TYPED_KIND(e) != TYPED_PASTE)
        {
            RING_SKIP(&typed_queue, 1);
            continue;
        }
        const uint8_t source = clip_paste_source(e & TYPED_PASTE_SOURCE);
        RING_SKIP(&typed_queue, 1);
        if (!clip_size(source))
            continue;
        typed_paste_from = (uint8_t)(source + 1);
        typed_paste_at = 0;
        if (vterm.bracketed_paste && !(e & TYPED_PASTE_PLAIN))
            typed_paste_open = typed_paste_close = TYPED_BRACKET;
    }
}

/* The paste being typed comes first: its opening bracket, its text and
 * its closing bracket. Then the queue's bytes, as far as the first entry
 * that is no byte. */
int HOST_IN_RAM_FUNC typed_peek(size_t offset)
{
    typed_front();
    if (offset < typed_paste_open)
        return (unsigned char)typed_open_bracket[TYPED_BRACKET - typed_paste_open + offset];
    offset -= typed_paste_open;
    if (typed_paste_from)
    {
        const uint8_t source = (uint8_t)(typed_paste_from - 1);
        const size_t left = clip_size(source) - typed_paste_at;
        if (offset < left)
            return (unsigned char)clip_at(source, typed_paste_at + offset);
        offset -= left;
    }
    if (offset < typed_paste_close)
        return (unsigned char)typed_close_bracket[TYPED_BRACKET - typed_paste_close + offset];
    offset -= typed_paste_close;
    const uint32_t len = RING_LEN(&typed_queue);
    for (uint32_t i = 0; i < len; i++)
    {
        const typed_entry_t e = *RING_AT(&typed_queue, i);
        if (TYPED_KIND(e) != TYPED_BYTE)
            break;
        if (i == offset)
            return e;
    }
    return -1;
}

size_t HOST_IN_RAM_FUNC typed_read(char *buf, size_t length)
{
    size_t got = 0;
    int ch;
    for (; got < length && (ch = typed_peek(0)) >= 0; got++)
    {
        buf[got] = (char)ch;
        if (typed_paste_open)
            typed_paste_open--;
        else if (typed_paste_from)
        {
            if (++typed_paste_at == clip_size((uint8_t)(typed_paste_from - 1)))
                typed_paste_end();
        }
        else if (typed_paste_close)
            typed_paste_close--;
        else
            RING_SKIP(&typed_queue, 1);
    }
    return got;
}

size_t typed_free(void)
{
    return RING_AVAIL(&typed_queue);
}

/* Every entry waiting of kinds first to last is taken back. Plain text
 * is typing, no paste, and stays. */
static void typed_void(unsigned first, unsigned last)
{
    const uint32_t len = RING_LEN(&typed_queue);
    for (uint32_t i = 0; i < len; i++)
    {
        typed_entry_t *e = RING_AT(&typed_queue, i);
        if (TYPED_KIND(*e) >= first && TYPED_KIND(*e) <= last &&
            !(TYPED_KIND(*e) == TYPED_PASTE && (*e & TYPED_PASTE_PLAIN)))
            *e = TYPED_ENTRY(TYPED_VOID, 0);
    }
}

void typed_void_mouse(void)
{
    typed_void(TYPED_PRESS, TYPED_RELEASE);
}

/* A Ctrl-C: no paste waits, and the paste being typed has no more text.
 * Its closing bracket still comes when its opening one has begun to be
 * read, and neither when it has not. */
static void typed_break(void)
{
    ria_trigger_sigint();
    typed_void(TYPED_PASTE, TYPED_PASTE);
    if (!clip_paste_break())
        return;
    if (typed_paste_from)
        typed_paste_end();
    if (typed_paste_open == TYPED_BRACKET)
        typed_paste_open = typed_paste_close = 0;
}

/* The caller has checked the room. */
static void typed_bytes(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        RING_PUT(&typed_queue, (uint8_t)s[i]);
}

/* A Ctrl-C breaks before the space is checked, so a break is seen even
 * when the queue is too full to take the byte. */
void typed_push(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (s[i] == 0x03)
            typed_break();
    if (n <= RING_AVAIL(&typed_queue))
        typed_bytes(s, n);
}

bool typed_put(typed_entry_t entry)
{
    return RING_PUT(&typed_queue, entry);
}

void typed_cut(const char *s, size_t n)
{
    if (n >= RING_AVAIL(&typed_queue))
        return;
    RING_PUT(&typed_queue, TYPED_ENTRY(TYPED_CUT, n));
    typed_bytes(s, n);
}

/* A drag entry always follows its held entry, and the line editor takes
 * the held one first, so with a drag newest of two or more, the two
 * newest are a pair. */
void typed_drag(uint8_t held, uint8_t to)
{
    const typed_entry_t h = TYPED_ENTRY(TYPED_HELD, held), d = TYPED_ENTRY(TYPED_DRAG, to);
    const uint32_t len = RING_LEN(&typed_queue);
    if (len >= 2 && TYPED_KIND(*RING_AT(&typed_queue, len - 1)) == TYPED_DRAG)
    {
        *RING_AT(&typed_queue, len - 2) = h;
        *RING_AT(&typed_queue, len - 1) = d;
    }
    else
    {
        RING_PUT(&typed_queue, h);
        RING_PUT(&typed_queue, d);
    }
}

bool typed_entry(void)
{
    typed_front();
    return !typed_paste_open && !typed_paste_from && !typed_paste_close &&
           RING_LEN(&typed_queue) && TYPED_KIND(*RING_AT(&typed_queue, 0)) != TYPED_BYTE;
}

bool typed_take(typed_entry_t *entry)
{
    if (!typed_entry())
        return false;
    *entry = *RING_AT(&typed_queue, 0);
    RING_SKIP(&typed_queue, TYPED_KIND(*entry) == TYPED_CUT ? 1u + (*entry & 0xFF) : 1u);
    return true;
}

void typed_paste_unbracket(void)
{
    if (typed_paste_open == TYPED_BRACKET)
        typed_paste_open = 0;
    if (typed_paste_close == TYPED_BRACKET)
        typed_paste_close = 0;
}

void typed_clip_changed(uint8_t which)
{
    if (typed_paste_from == which + 1)
        typed_paste_end();
}

/* A failed load can leave the counters holding more than the queue: the
 * skip leaves them equal whatever they hold. */
void typed_clear(void)
{
    RING_SKIP(&typed_queue, RING_LEN(&typed_queue));
    typed_paste_from = typed_paste_open = typed_paste_close = 0;
    clip_paste_drop();
}

/* The queue is saved from its front. The host's text is never saved, and
 * every load ends it: a paste of it still waiting is saved as an entry
 * taken back, and the one being typed as a paste with no more text, whose
 * closing bracket still comes when the program has begun to read its
 * opening one, and whose brackets go when it has not. A paste of a machine
 * buffer is saved as it is, to go on from where it is in the buffer. */
static void typed_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    const uint32_t len = RING_LEN(&typed_queue);
    const bool own = typed_paste_from && typed_paste_from <= CLIP_COUNT;
    const bool unseen = typed_paste_from && !own && typed_paste_open == TYPED_BRACKET;
    sst_put_u16(c, (uint16_t)len);
    for (uint32_t i = 0; i < TYPED_QUEUE_SIZE; i++)
    {
        typed_entry_t e = i < len ? *RING_AT(&typed_queue, i) : 0;
        if (TYPED_KIND(e) == TYPED_PASTE && (e & TYPED_PASTE_SOURCE) == CLIP_HOST)
            e = TYPED_ENTRY(TYPED_VOID, 0);
        sst_put_u16(c, e);
    }
    sst_put_u8(c, own ? typed_paste_from : 0);
    sst_put_u16(c, own ? typed_paste_at : 0);
    sst_put_u8(c, unseen ? 0 : typed_paste_open);
    sst_put_u8(c, unseen ? 0 : typed_paste_close);
}

/* An entry is of a kind the queue has, a byte is a byte, a paste is of a
 * machine buffer, a cut key's bytes are there behind its mark, and a drag
 * behind another entry follows its held entry, as typed_drag counts on. The
 * paste being typed is at text its buffer holds, which CLIP's chunk loaded
 * before this one (drivers.h). */
static bool typed_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    typed_clear();
    const uint16_t len = sst_get_u16(c);
    typed_entry_t entries[TYPED_QUEUE_SIZE];
    for (uint32_t i = 0; i < TYPED_QUEUE_SIZE; i++)
        entries[i] = sst_get_u16(c);
    const uint8_t from = sst_get_u8(c);
    const uint16_t at = sst_get_u16(c);
    const uint8_t open = sst_get_u8(c);
    const uint8_t close = sst_get_u8(c);
    if (!sst_ok(c))
        return false;
    if (len > TYPED_QUEUE_SIZE)
        return sst_refuse(c, "a queue holding more than it can");
    for (uint32_t i = 0, bytes = 0; i < len; i++)
    {
        const typed_entry_t e = entries[i];
        const unsigned arg = e & 0xFFF;
        if (TYPED_KIND(e) > TYPED_RELEASE ||
            (TYPED_KIND(e) == TYPED_BYTE ? arg > 0xFF : bytes != 0) ||
            (TYPED_KIND(e) == TYPED_PASTE && arg >= CLIP_COUNT) ||
            (TYPED_KIND(e) == TYPED_CUT && arg >= len - i) ||
            (TYPED_KIND(e) == TYPED_DRAG && i && TYPED_KIND(entries[i - 1]) != TYPED_HELD))
            return sst_refuse(c, "an entry the queue cannot hold");
        bytes = TYPED_KIND(e) == TYPED_CUT ? arg : bytes ? bytes - 1 : 0;
    }
    if (from > CLIP_COUNT || open > TYPED_BRACKET || close > TYPED_BRACKET ||
        (from ? at >= clip_size((uint8_t)(from - 1)) : at != 0))
        return sst_refuse(c, "a paste from nowhere");
    for (uint32_t i = 0; i < len; i++)
        RING_PUT(&typed_queue, entries[i]);
    typed_paste_from = from;
    typed_paste_at = at;
    typed_paste_open = open;
    typed_paste_close = close;
    return true;
}

#define TYPED_SST(X)                                                    \
    X(FN, typed_sst_save, typed_sst_load, 2 + 2 * TYPED_QUEUE_SIZE + 5, 1) \
    X(IN_FN, typed_queue)                                               \
    X(IN_FN, typed_paste_from)                                          \
    X(IN_FN, typed_paste_at)                                            \
    X(IN_FN, typed_paste_open)                                          \
    X(IN_FN, typed_paste_close)
SST_TABLE(typed, TYPED_SST)
