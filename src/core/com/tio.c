/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/com/tio.h"
#include "core/sys/timer.h"
#include "machine.h"
#include <ctype.h>
#include <string.h>

#define TIO_ESC 0x1B
#define TIO_CAN 0x18
#define TIO_SUB 0x1A
#define TIO_DEL 0x7F

/* The framing states. TIO_FRAME_NUM is a CSI with only digits so far, where
 * '$' ends rxvt's shifted keys. TIO_FRAME_ONE waits for the one byte after
 * ESC 'N' or ESC '[' '['. The mouse states count down the three bytes of an
 * X10 mouse report, one left at TIO_FRAME_MOUSE. */
#define TIO_FRAME_GROUND 0
#define TIO_FRAME_ESC 1
#define TIO_FRAME_CSI 2
#define TIO_FRAME_NUM 3
#define TIO_FRAME_PARAM 4
#define TIO_FRAME_INTER 5
#define TIO_FRAME_SS3 6
#define TIO_FRAME_ONE 7
#define TIO_FRAME_MOUSE 8
#define TIO_FRAME_LAST (TIO_FRAME_MOUSE + 2)

/* The open length that stands for the rest of an overlong sequence, which
 * no prefix reaches. */
#define TIO_OPEN_OVERLONG UINT8_MAX
_Static_assert(TIO_ITEM_MAX < TIO_OPEN_OVERLONG, "no prefix is mistaken for an overlong rest");

typedef struct
{
    uint8_t overlong;   // framing state inside an overlong sequence, 0 for none
    uint8_t open;       // length of the unfinished sequence last seen in front
    timer_mach_t quiet; // when that unfinished sequence has been quiet enough
} tio_source_t;

static tio_source_t tio_sources[COM_SOURCE_COUNT];

/* One byte inside a sequence. ESC starts the sequence over, CAN and SUB
 * cancel it, and any other control, DEL, or a byte of 0x80 and up ends it
 * and stands alone. After a lone ESC the controls and DEL make two-byte
 * Alt keys instead. The three bytes of an X10 mouse report can be anything
 * from 0x20 up. */
static tio_step_t HOST_IN_RAM_FUNC tio_frame_in(uint8_t was, uint8_t *frame, uint8_t ch)
{
    if (ch == TIO_CAN || ch == TIO_SUB)
        return TIO_DROP;
    if (ch == TIO_ESC)
        return TIO_AGAIN;
    if (was >= TIO_FRAME_MOUSE)
    {
        if (ch < 0x20)
            return TIO_AGAIN;
        if (was > TIO_FRAME_MOUSE)
            *frame = was - 1;
        return *frame ? TIO_OPEN : TIO_END;
    }
    if (ch >= 0x80 || (was != TIO_FRAME_ESC && (ch < 0x20 || ch == TIO_DEL)))
        return TIO_AGAIN;
    bool digit = ch >= '0' && ch <= '9';
    bool param = ch >= '0' && ch <= '?';
    bool inter = ch >= ' ' && ch <= '/';
    switch (was)
    {
    case TIO_FRAME_ESC:
        if (ch == '[')
            *frame = TIO_FRAME_CSI;
        else if (ch == 'O')
            *frame = TIO_FRAME_SS3;
        else if (ch == 'N')
            *frame = TIO_FRAME_ONE;
        break;
    case TIO_FRAME_CSI:
        if (ch == '[')
            *frame = TIO_FRAME_ONE;
        else if (ch == 'M')
            *frame = TIO_FRAME_MOUSE + 2;
        else if (digit)
            *frame = TIO_FRAME_NUM;
        else if (param)
            *frame = TIO_FRAME_PARAM;
        else if (inter)
            *frame = TIO_FRAME_INTER;
        break;
    case TIO_FRAME_NUM:
        if (digit)
            *frame = TIO_FRAME_NUM;
        else if (param)
            *frame = TIO_FRAME_PARAM;
        else if (inter && ch != '$')
            *frame = TIO_FRAME_INTER;
        break;
    case TIO_FRAME_PARAM:
        if (param)
            *frame = TIO_FRAME_PARAM;
        else if (inter)
            *frame = TIO_FRAME_INTER;
        break;
    case TIO_FRAME_INTER:
        /* A parameter byte after an intermediate is out of place, and the
         * sequence is kept to its final byte, where tio_csi_parse refuses
         * it. */
        if (param || inter)
            *frame = TIO_FRAME_INTER;
        break;
    case TIO_FRAME_SS3:
        if (digit || ch == ';')
            *frame = TIO_FRAME_SS3;
        break;
    }
    return *frame ? TIO_OPEN : TIO_END;
}

tio_step_t HOST_IN_RAM_FUNC tio_frame(uint8_t *frame, uint8_t ch)
{
    uint8_t was = *frame;
    *frame = TIO_FRAME_GROUND;
    if (was)
        return tio_frame_in(was, frame, ch);
    if (ch != TIO_ESC)
        return TIO_END;
    *frame = TIO_FRAME_ESC;
    return TIO_OPEN;
}

bool HOST_IN_RAM_FUNC tio_csi_parse(const char *item, size_t len, tio_csi_t *csi)
{
    memset(csi, 0, sizeof *csi);
    if (len < 3 || item[1] != '[')
        return false;
    size_t i = 2;
    uint8_t ch = (uint8_t)item[i];
    if (ch >= '<' && ch <= '?')
    {
        csi->priv = ch;
        i++;
    }
    /* Digits only, the one form rxvt's '$' keys take. A ':' starts a
     * subparameter, which is skipped. */
    bool digits = !csi->priv;
    bool sub = false;
    for (; i < len && (ch = (uint8_t)item[i]) >= '0' && ch <= '?'; i++)
    {
        if (isdigit(ch))
        {
            if (!sub && csi->count < 2)
            {
                uint32_t v = (uint32_t)csi->param[csi->count] * 10 + (ch - '0');
                csi->param[csi->count] = v > UINT16_MAX ? UINT16_MAX : (uint16_t)v;
            }
            continue;
        }
        digits = false;
        if (ch == ';')
        {
            csi->count++;
            sub = false;
        }
        else if (ch == ':')
            sub = true;
        else
            return false; // a private marker after the first parameter byte
    }
    csi->count++;
    for (; i < len && (ch = (uint8_t)item[i]) >= ' ' && ch <= '/'; i++)
        csi->inter = ch;
    if (i == len)
    {
        /* Only rxvt's shifted keys end on an intermediate: digits, then '$'. */
        if (!digits || csi->inter != '$' || len < 4 || !isdigit((uint8_t)item[len - 2]))
            return false;
        csi->inter = 0;
        csi->final = '$';
        return true;
    }
    csi->final = (uint8_t)item[i++];
    if (csi->final < '@' || csi->final > '~')
        return false;
    /* An X10 mouse report has three bytes after its 'M', and a Linux console
     * function key one after its second '['. */
    if (i < len)
        return i == 3 && len - i == (csi->final == 'M' ? 3u : csi->final == '[' ? 1u : 0u);
    return true;
}

/* How long an item at offset start in src may grow: TIO_ITEM_MAX, or less
 * when the source's queue holds fewer. The byte staged from src counts only
 * for the item it starts. */
static size_t HOST_IN_RAM_FUNC tio_limit(com_source_t src, size_t start)
{
    size_t limit = com_queue_size(src) + (!start && com_rx_peek(src) >= 0);
    return limit < TIO_ITEM_MAX ? limit : TIO_ITEM_MAX;
}

/* True once the unfinished sequence in front of t, open bytes long, has
 * gone TIO_ESC_MS without growing. With mark, a length it has not been seen
 * at before starts the wait over. */
static bool HOST_IN_RAM_FUNC tio_quiet(tio_source_t *t, uint8_t open, bool mark)
{
    if (t->open != open)
    {
        if (mark)
        {
            t->open = open;
            t->quiet = timer_mach_in_ms(TIO_ESC_MS);
        }
        return false;
    }
    return timer_mach_passed(t->quiet);
}

/* Passes over the n bytes in front of the scan, which make no item. With
 * buf they are taken, and the scan starts again at the front. Returns where
 * it starts. */
static size_t HOST_IN_RAM_FUNC tio_pass(com_source_t src, char *buf, size_t n)
{
    if (!buf)
        return n;
    while (n)
    {
        n -= com_read(src, buf, n < TIO_ITEM_MAX ? n : TIO_ITEM_MAX);
        tio_sources[src].open = 0;
    }
    return 0;
}

/* The next whole item in src, found by the one scan tio_read and tio_idle
 * share. With buf the item is taken into it, and so is everything in front
 * of it: a dropped sequence, the bytes before one that starts the framing
 * over, and an overlong one, its rest taken as it arrives. Without buf
 * nothing is taken. A sequence that has not ended is left where it is
 * until TIO_ESC_MS of quiet, when a lone ESC is the Escape key and any
 * other is dropped. Only taking starts the wait, so without buf a lone ESC
 * is an item once a read has seen it quiet. */
static size_t HOST_IN_RAM_FUNC tio_scan(com_source_t src, char *buf)
{
    tio_source_t *t = &tio_sources[src];
    uint8_t frame = t->overlong;
    bool overlong = frame != TIO_FRAME_GROUND;
    size_t start = 0;
    size_t n = 0;
    for (;;)
    {
        tio_step_t step;
        int c = com_peek(src, start + n);
        if (c >= 0)
        {
            step = tio_frame(&frame, (uint8_t)c);
            if (step == TIO_AGAIN)
            {
                start = tio_pass(src, buf, start + n);
                n = 0;
                overlong = false;
                step = tio_frame(&frame, (uint8_t)c);
            }
            n++;
        }
        else if (overlong)
        {
            start = tio_pass(src, buf, start + n);
            if (buf && tio_quiet(t, TIO_OPEN_OVERLONG, true))
                overlong = false;
            break;
        }
        else if (!n || start || !tio_quiet(t, (uint8_t)n, buf))
            break;
        else /* Only an ESC is open at one byte. */
            step = n == 1 ? TIO_END : TIO_DROP;
        if (step == TIO_END && !overlong)
        {
            if (buf)
            {
                com_read(src, buf, n);
                t->open = 0;
            }
            return n;
        }
        if (step != TIO_OPEN)
        {
            start = tio_pass(src, buf, start + n);
            n = 0;
            overlong = false;
        }
        else if (n >= tio_limit(src, start))
            overlong = true;
    }
    if (buf)
        t->overlong = overlong ? frame : TIO_FRAME_GROUND;
    return 0;
}

size_t HOST_IN_RAM_FUNC tio_read(com_source_t *src, char *buf)
{
    for (com_source_t s = COM_SOURCE_KEYBOARD; s < COM_SOURCE_COUNT; s++)
    {
        size_t n = tio_scan(s, buf);
        if (n)
        {
            *src = s;
            return n;
        }
    }
    return 0;
}

bool HOST_IN_RAM_FUNC tio_idle(void)
{
    for (com_source_t s = COM_SOURCE_KEYBOARD; s < COM_SOURCE_COUNT; s++)
        if (tio_scan(s, NULL))
            return false;
    return true;
}

void HOST_IN_RAM_FUNC tio_forget(com_source_t src)
{
    memset(&tio_sources[src], 0, sizeof tio_sources[src]);
}

void tio_read_end(void)
{
    char scratch[TIO_ITEM_MAX];
    for (com_source_t s = COM_SOURCE_KEYBOARD; s < COM_SOURCE_COUNT; s++)
    {
        tio_source_t *t = &tio_sources[s];
        size_t n = 0;
        int c;
        while (t->overlong && (c = com_peek(s, n)) >= 0)
            if (tio_frame(&t->overlong, (uint8_t)c) != TIO_AGAIN)
                n++;
        tio_pass(s, scratch, n);
        t->overlong = TIO_FRAME_GROUND;
    }
}

void tio_init(void)
{
    memset(tio_sources, 0, sizeof tio_sources);
}

void tio_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    for (com_source_t s = COM_SOURCE_KEYBOARD; s < COM_SOURCE_COUNT; s++)
    {
        sst_put_u8(c, tio_sources[s].overlong);
        sst_put_u8(c, tio_sources[s].open);
        sst_put_u64(c, tio_sources[s].quiet);
    }
}

bool tio_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    tio_source_t sources[COM_SOURCE_COUNT];
    for (com_source_t s = COM_SOURCE_KEYBOARD; s < COM_SOURCE_COUNT; s++)
    {
        sources[s].overlong = sst_get_u8(c);
        sources[s].open = sst_get_u8(c);
        sources[s].quiet = sst_get_u64(c);
    }
    if (!sst_ok(c))
        return false;
    for (com_source_t s = COM_SOURCE_KEYBOARD; s < COM_SOURCE_COUNT; s++)
        if (sources[s].overlong > TIO_FRAME_LAST)
            return false;
    memcpy(tio_sources, sources, sizeof tio_sources);
    return true;
}
