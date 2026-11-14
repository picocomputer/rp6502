/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/com/tio.h"
#include "core/com/manifold.h"
#include "core/term/cells.h"
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

/* Insert mode goes off at every read, and stays off, because the line
 * editor overwrites the line as it draws it again. */
#define TIO_MODES "\33[4l"

typedef struct
{
    uint8_t overlong;   // framing state inside an overlong sequence, 0 for none
    uint8_t open;       // length of the unfinished sequence last seen in front
    timer_mach_t quiet; // when that unfinished sequence has been quiet enough
} tio_source_t;

static tio_source_t tio_sources[COM_SOURCE_COUNT];

static uint16_t tio_pinned_width;
static uint16_t tio_pinned_height;

/* A program's own size for the console's terminal, while either is pinned:
 * a third grid, which the line editor and the terminal's answers use. */
static term_grid_t tio_grid;
static uint8_t tio_grid_lines[2 * TIO_GRID_ROWS];

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

/* How long a sequence at the front of src may grow: TIO_ITEM_MAX, or less
 * when the source's queue, with the byte staged from it, holds fewer. */
static size_t HOST_IN_RAM_FUNC tio_limit(com_source_t src)
{
    size_t limit = com_queue_size(src) + (com_rx_peek(src) >= 0);
    return limit < TIO_ITEM_MAX ? limit : TIO_ITEM_MAX;
}

/* True once the unfinished sequence in front of t, open bytes long, has
 * gone TIO_ESC_MS without growing. A length it has not been seen at before
 * starts the wait over. */
static bool HOST_IN_RAM_FUNC tio_quiet_open(tio_source_t *t, uint8_t open)
{
    if (t->open != open)
    {
        t->open = open;
        t->quiet = timer_mach_in_ms(TIO_ESC_MS);
        return false;
    }
    return timer_mach_passed(t->quiet);
}

/* Takes what has arrived of the overlong sequence at the front of src. True
 * once its end has been taken, when it has been quiet long enough to be
 * given up, or when src is in none. A byte that starts the framing over is
 * left for the next item. */
static bool HOST_IN_RAM_FUNC tio_skip(com_source_t src, char *scratch)
{
    tio_source_t *t = &tio_sources[src];
    while (t->overlong)
    {
        size_t n = 0;
        int c;
        while (t->overlong && n < TIO_ITEM_MAX && (c = com_peek(src, n)) >= 0)
            if (tio_frame(&t->overlong, (uint8_t)c) != TIO_AGAIN)
                n++;
        if (n)
        {
            com_read(src, scratch, n);
            t->open = 0;
        }
        else if (t->overlong)
        {
            if (!tio_quiet_open(t, TIO_OPEN_OVERLONG))
                return false;
            t->overlong = TIO_FRAME_GROUND;
        }
    }
    return true;
}

/* The next whole item at the front of src. A sequence that has not ended
 * is left where it is until TIO_ESC_MS of quiet, when a lone ESC is the
 * Escape key and any other is dropped. An overlong one is skipped. */
static size_t HOST_IN_RAM_FUNC tio_take(com_source_t src, char *buf)
{
    tio_source_t *t = &tio_sources[src];
    while (tio_skip(src, buf))
    {
        size_t limit = tio_limit(src);
        uint8_t frame = TIO_FRAME_GROUND;
        tio_step_t step;
        size_t n = 0;
        for (;;)
        {
            int c = com_peek(src, n);
            if (c < 0)
            {
                if (!tio_quiet_open(t, (uint8_t)n) || !n)
                    return 0;
                /* Only an ESC is open at one byte. */
                step = n == 1 ? TIO_END : TIO_DROP;
                break;
            }
            step = tio_frame(&frame, (uint8_t)c);
            if (step == TIO_AGAIN)
                break;
            buf[n++] = (char)c;
            if (step != TIO_OPEN || n >= limit)
                break;
        }
        /* An item, a dropped or given up sequence, or the bytes in front of
         * one that starts the framing over, are taken. An overlong sequence
         * leaves the frame in the state its rest is skipped from. */
        com_read(src, buf, n);
        t->open = 0;
        if (step == TIO_END)
            return n;
        if (step == TIO_OPEN)
            t->overlong = frame;
    }
    return 0;
}

size_t HOST_IN_RAM_FUNC tio_read(com_source_t *src, char *buf)
{
    for (com_source_t s = COM_SOURCE_KEYBOARD; s < COM_SOURCE_COUNT; s++)
    {
        size_t n = tio_take(s, buf);
        if (n)
        {
            *src = s;
            return n;
        }
    }
    return 0;
}

/* Whether src holds a whole item, found as tio_take would find it but
 * without taking anything. A lone ESC in front is one once tio_take has
 * seen it quiet long enough. */
static bool HOST_IN_RAM_FUNC tio_waiting(com_source_t src)
{
    const tio_source_t *t = &tio_sources[src];
    size_t limit = tio_limit(src);
    uint8_t frame = t->overlong;
    bool overlong = frame != TIO_FRAME_GROUND;
    size_t start = 0;
    for (size_t at = 0;; at++)
    {
        int c = com_peek(src, at);
        if (c < 0)
            return at == 1 && !overlong && t->open == 1 &&
                   timer_mach_passed(t->quiet);
        tio_step_t step = tio_frame(&frame, (uint8_t)c);
        if (step == TIO_AGAIN)
        {
            overlong = false;
            start = at;
            step = tio_frame(&frame, (uint8_t)c);
        }
        if (step == TIO_END && !overlong)
            return true;
        if (step != TIO_OPEN)
        {
            overlong = false;
            start = at + 1;
        }
        else if (at + 1 - start >= limit)
            overlong = true;
    }
}

bool HOST_IN_RAM_FUNC tio_idle(void)
{
    for (com_source_t s = COM_SOURCE_KEYBOARD; s < COM_SOURCE_COUNT; s++)
        if (tio_waiting(s))
            return false;
    return true;
}

void tio_read_start(tio_line_t *line)
{
    term_t *t = &cells_term;
    com_flush();
    manifold_write_to(COM_TERM_ALL, TIO_MODES, sizeof TIO_MODES - 1);
    if (term_pos(t, term_shown(t))->wrap_pending)
    {
        com_putchar('\r');
        com_putchar('\n');
        com_flush();
    }
    const term_grid_t *g = term_shown(t);
    const term_pos_t *p = term_pos(t, g);
    line->col = (uint8_t)(p->x + 1);
    line->cols = (uint8_t)term_cols(t, g, p->y);
    line->width = g->width;
    line->height = g->height;
}

uint16_t tio_get_term_width(void)
{
    return term_shown(&cells_term)->width;
}

uint16_t tio_get_term_height(void)
{
    return term_shown(&cells_term)->height;
}

static bool tio_has_grid(void)
{
    return cells_term.grid_count > TERM_SCREEN_GRIDS;
}

/* The third grid starts as a copy of the showing one and takes the pinned
 * size. An axis not pinned takes the showing grid's when the other is
 * pinned, and keeps it: the program's terminal is a fixed size until both
 * are 0 again, when it is dropped. The screen's grids are never touched. */
static void tio_size_grid(void)
{
    term_t *t = &cells_term;
    if (!tio_pinned_width && !tio_pinned_height)
    {
        if (tio_has_grid())
            term_drop_grid(t, &tio_grid);
        return;
    }
    const term_grid_t *shown = cells_shown(t);
    if (!tio_has_grid())
        term_add_grid(t, &tio_grid, shown, tio_grid_lines, TIO_GRID_ROWS);
    term_set_width(t, &tio_grid, (uint8_t)(tio_pinned_width ? tio_pinned_width : shown->width));
    term_set_height(t, &tio_grid, (uint8_t)(tio_pinned_height ? tio_pinned_height : shown->height));
}

void tio_set_term_width(uint16_t v)
{
    tio_pinned_width = v;
    tio_size_grid();
}

void tio_set_term_height(uint16_t v)
{
    tio_pinned_height = v;
    tio_size_grid();
}

void HOST_IN_RAM_FUNC tio_forget(com_source_t src)
{
    memset(&tio_sources[src], 0, sizeof tio_sources[src]);
}

void tio_cursor_shape(uint8_t ps)
{
    char seq[] = "\33[0 q";
    seq[2] = (char)('0' + ps);
    manifold_write_to(COM_TERM_SCREEN, seq, sizeof seq - 1);
}

void tio_cursor_default(void)
{
    tio_cursor_shape(0);
}

void tio_read_end(void)
{
    char scratch[TIO_ITEM_MAX];
    for (com_source_t s = COM_SOURCE_KEYBOARD; s < COM_SOURCE_COUNT; s++)
    {
        tio_skip(s, scratch);
        tio_sources[s].overlong = TIO_FRAME_GROUND;
    }
}

void tio_stop(void)
{
    tio_read_end();
    tio_pinned_width = 0;
    tio_pinned_height = 0;
    tio_size_grid();
}

void tio_init(void)
{
    memset(tio_sources, 0, sizeof tio_sources);
    tio_pinned_width = 0;
    tio_pinned_height = 0;
    term_grid_init(&tio_grid, 80, tio_grid_lines, TIO_GRID_ROWS);
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
    sst_put_u16(c, tio_pinned_width);
    sst_put_u16(c, tio_pinned_height);
    sst_put_bool(c, tio_has_grid());
    term_grid_sst_put(c, &tio_grid);
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
    uint16_t pin_w = sst_get_u16(c), pin_h = sst_get_u16(c);
    bool grid = sst_get_bool(c);
    if (grid && !tio_has_grid())
        term_add_grid(&cells_term, &tio_grid, cells_shown(&cells_term),
                      tio_grid_lines, TIO_GRID_ROWS);
    else if (!grid && tio_has_grid())
        term_drop_grid(&cells_term, &tio_grid);
    if (!term_grid_sst_get(c, &tio_grid))
        return false;
    if (!sst_ok(c))
        return false;
    for (com_source_t s = COM_SOURCE_KEYBOARD; s < COM_SOURCE_COUNT; s++)
        if (sources[s].overlong > TIO_FRAME_LAST)
            return false;
    memcpy(tio_sources, sources, sizeof tio_sources);
    tio_pinned_width = pin_w;
    tio_pinned_height = pin_h;
    return true;
}
