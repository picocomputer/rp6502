/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/com/tio.h"
#include "core/com/manifold.h"
#include "core/sys/timer.h"
#include "core/vga/vga.h"
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

/* The pieces of the questions a read asks. Save and restore bracket them so
 * the cursor ends where it started. Autowrap goes on and insert mode off at
 * every read of the terminal in charge, and they stay that way, because the
 * line editor lets the terminal wrap the line and overwrites it as it draws
 * it again. */
#define TIO_HIDE "\33[?25l"
#define TIO_SHOW "\33[?25h"
#define TIO_MODES "\33[?7h\33[4l"
#define TIO_SAVE "\0337"
#define TIO_CPR "\33[6n"
#define TIO_PROBE "\33[>c\0338"
#define TIO_SIZE "\33[999;999H" TIO_CPR "\0338"

/* TIO_HELD is TIO_WAIT after a line end that waits for the terminal in
 * charge, when only answers are taken. */
typedef enum
{
    TIO_WAIT,
    TIO_HELD,
    TIO_READY,
} tio_state_t;

typedef struct
{
    uint8_t overlong;   // framing state inside an overlong sequence, 0 for none
    uint8_t answered;   // CPRs answered this read
    bool da2;           // answered DA2 this read
    uint8_t owed;       // CPRs asked and not yet answered, by any read
    uint8_t open;       // length of the unfinished sequence last seen in front
    timer_mach_t quiet; // when that unfinished sequence has been quiet enough
} tio_source_t;

static tio_source_t tio_sources[COM_SOURCE_COUNT];

static tio_state_t tio_state;
static timer_mach_t tio_deadline;
static timer_mach_t tio_quiet;
static uint8_t tio_charge; // COM_TERM_ bit of the terminal in charge, 0 for none
static bool tio_probe;     // the read asked the terminal in charge DA2
static bool tio_sized;     // the read asked the terminal in charge for the size

/* The latest prompt column, the largest column and the largest row the
 * terminal in charge reported. */
static uint16_t tio_pcol;
static uint16_t tio_width;
static uint16_t tio_height;

static uint16_t tio_pinned_width;
static uint16_t tio_pinned_height;
static uint16_t tio_naws_width;
static uint16_t tio_naws_height;

/* The cursor shape the line editor asked for, and the COM_TERM_ bits of the
 * terminals taken to do shapes, of those that have answered a CPR, and of
 * those that have had a read ready while in charge. */
static uint8_t tio_shape;
static uint8_t tio_marks;
static uint8_t tio_answers;
static uint8_t tio_greeted;

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

/* Reads an answer to CPR or DA2 from src into csi. False for any other item.
 * A CPR looks like a function key with a modifier, such as Shift-F3's
 * CSI 1;2R, so it is an answer only while src owes one. */
static bool HOST_IN_RAM_FUNC tio_is_answer(com_source_t src, const char *item,
                                           size_t len, tio_csi_t *csi)
{
    return tio_csi_parse(item, len, csi) && !csi->inter &&
           ((!csi->priv && csi->final == 'R' && csi->count == 2 &&
             tio_sources[src].owed) ||
            (csi->priv == '>' && csi->final == 'c'));
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

/* The source the terminal in charge answers on: telnet's, or the UART's,
 * where term.c answers too. */
static com_source_t HOST_IN_RAM_FUNC tio_charge_source(void)
{
    return tio_charge == COM_TERM_TEL ? COM_SOURCE_TEL : COM_SOURCE_UART;
}

/* The terminal in charge has answered every CPR this read asked it: the
 * prompt's, and the plain UART terminal's size. */
static bool HOST_IN_RAM_FUNC tio_answered(void)
{
    return tio_sources[tio_charge_source()].answered >= (tio_sized ? 2 : 1);
}

/* Geometry becomes ready, keeping what the terminal in charge reported only
 * when it has answered. With both sizes pinned the plain UART terminal is
 * asked no marker, so one that has answered its CPR and not DA2 by now is
 * marked as without shapes. */
static void HOST_IN_RAM_FUNC tio_set_ready(bool answered)
{
    tio_source_t *t = &tio_sources[tio_charge_source()];
    tio_state = TIO_READY;
    tio_greeted |= tio_charge;
    if (!answered)
        tio_pcol = 0;
    if (tio_charge == COM_TERM_UART && tio_probe && !tio_sized &&
        t->answered && !t->da2)
        tio_marks &= (uint8_t)~COM_TERM_UART;
}

/* The next whole item at the front of src. A sequence that has not ended
 * is left where it is until TIO_ESC_MS of quiet, when a lone ESC is the
 * Escape key and any other is dropped. An overlong one is skipped. While a
 * line end is held, only an answer is taken, and any other item at the front
 * of the source the terminal in charge answers on ends the wait, because an
 * answer still to come would arrive behind it. What is no item is taken
 * either way. */
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
        tio_csi_t csi;
        if (tio_state == TIO_HELD && step == TIO_END &&
            !tio_is_answer(src, buf, n, &csi))
        {
            if (src == tio_charge_source())
                tio_set_ready(tio_answered());
            return 0;
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

/* Writes DECSCUSR with the one-digit parameter ps to the terminals in to. */
static void tio_write_shape(uint8_t to, uint8_t ps)
{
    char seq[] = "\33[0 q";
    seq[2] = (char)('0' + ps);
    if (to)
        manifold_write_to(to, seq, sizeof seq - 1);
}

/* The terminals the line editor's cursor shape goes to. */
static uint8_t tio_shaped(void)
{
    uint8_t to = tio_marks & manifold_terms_connected();
    if (tio_charge == COM_TERM_UART && !tio_sources[COM_SOURCE_UART].da2)
        to &= (uint8_t)~COM_TERM_UART;
    return to;
}

/* Any CPR answer marks its terminal as one that answers. Only the terminal
 * in charge gives geometry. Its prompt answer comes first, and the plain UART
 * terminal's size answer second; an answer an earlier read was owed can come
 * ahead of them. 999;999 clamps the size answer to the bottom right, so it
 * has the largest column, and an answer left of that is a prompt answer, the
 * latest one winning. Without the size question every answer is a prompt
 * answer. Once geometry is ready without a prompt column, the answers change
 * nothing. */
static void HOST_IN_RAM_FUNC tio_cpr(com_source_t src, const tio_csi_t *csi)
{
    tio_source_t *t = &tio_sources[src];
    t->owed--;
    if (t->answered < UINT8_MAX)
        t->answered++;
    tio_answers |= COM_TERM(src);
    if (src != tio_charge_source())
        return;
    /* The size CPR is the marker, asked after DA2. A terminal that does DA2
     * answers it before the marker, so a marker answer with none before it
     * marks the terminal as without shapes. */
    if (tio_sized && tio_probe && t->answered == 2 && !t->da2)
        tio_marks &= (uint8_t)~COM_TERM(src);
    if (tio_state == TIO_READY && !tio_pcol)
        return;
    uint16_t col = csi->param[1] ? csi->param[1] : 1;
    if (!tio_sized)
    {
        tio_pcol = col;
        return;
    }
    if (col < tio_width)
        tio_pcol = col;
    else if (col > tio_width)
    {
        if (!tio_pcol)
            tio_pcol = tio_width;
        tio_width = col;
    }
    if (csi->param[0] > tio_height)
        tio_height = csi->param[0];
}

/* Any DA2 answer marks its terminal as supporting shapes. One that comes
 * after geometry is ready gets the current shape at once, when it is a
 * terminal the shapes go to. */
static void HOST_IN_RAM_FUNC tio_da2(com_source_t src)
{
    uint8_t term = COM_TERM(src);
    tio_marks |= term;
    tio_sources[src].da2 = true;
    if (tio_state == TIO_READY && (tio_shaped() & term))
        tio_write_shape(term, tio_shape);
}

/* Takes a terminal's answer to CPR or DA2. True when the item was one. The
 * keyboard owes no CPR, and a DA2 answer from it is dropped. */
static bool HOST_IN_RAM_FUNC tio_answer(com_source_t src, const char *item, size_t len)
{
    tio_csi_t csi;
    if (!tio_is_answer(src, item, len, &csi))
        return false;
    if (src != COM_SOURCE_KEYBOARD)
    {
        if (csi.final == 'R')
            tio_cpr(src, &csi);
        else
            tio_da2(src);
    }
    return true;
}

/* When tio_take ends the wait for a held line end, the read returns nothing,
 * so the line editor takes the held line before anything typed after it. */
size_t HOST_IN_RAM_FUNC tio_read(com_source_t *src, char *buf)
{
    bool held = tio_state == TIO_HELD;
    com_source_t s = COM_SOURCE_KEYBOARD;
    while (s < COM_SOURCE_COUNT)
    {
        size_t n = tio_take(s, buf);
        if (!n)
        {
            if (held && tio_state == TIO_READY)
                return 0;
            s++;
            continue;
        }
        if (tio_state != TIO_READY)
            tio_quiet = timer_mach_in_ms(TIO_QUIET_MS);
        if (!tio_answer(s, buf, n))
        {
            *src = s;
            return n;
        }
        s = COM_SOURCE_KEYBOARD;
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

static size_t tio_cat(char *buf, size_t n, const char *s)
{
    size_t len = strlen(s);
    memcpy(&buf[n], s, len);
    return n + len;
}

/* Starts a read with charge, a COM_TERM_ bit or 0, as the terminal in charge. */
static void tio_start(uint8_t length, uint8_t charge)
{
    tio_charge = charge;
    tio_probe = length > 0 &&
                (tio_charge == COM_TERM_UART ||
                 (tio_charge == COM_TERM_TEL && !(tio_marks & COM_TERM_TEL)));
    tio_sized = tio_charge == COM_TERM_UART &&
                !(tio_pinned_width && tio_pinned_height);
    tio_state = tio_charge ? TIO_WAIT : TIO_READY;
    tio_deadline = timer_mach_in_ms(TIO_DEADLINE_MS);
    tio_quiet = tio_deadline;
    tio_pcol = 0;
    tio_width = 0;
    tio_height = 0;
    for (com_source_t s = COM_SOURCE_UART; s < COM_SOURCE_COUNT; s++)
    {
        tio_sources[s].answered = 0;
        tio_sources[s].da2 = false;
    }
    if (!tio_charge)
        return;
    tio_source_t *t = &tio_sources[tio_charge_source()];
    uint8_t asked = tio_sized ? 2 : 1;
    t->owed = t->owed > UINT8_MAX - asked ? UINT8_MAX : (uint8_t)(t->owed + asked);
    char burst[sizeof(TIO_HIDE TIO_MODES TIO_SAVE TIO_CPR TIO_PROBE TIO_SIZE TIO_SHOW)];
    size_t n = tio_cat(burst, 0, TIO_HIDE TIO_MODES TIO_SAVE TIO_CPR);
    if (tio_probe)
        n = tio_cat(burst, n, TIO_PROBE);
    if (tio_sized)
        n = tio_cat(burst, n, TIO_SIZE);
    n = tio_cat(burst, n, TIO_SHOW);
    manifold_write_to(tio_charge, burst, n);
}

void tio_read_start(uint8_t length)
{
    /* A question longer than the room left in the console's queue would go
     * out ahead of the end of what a program printed. The flush can end a
     * telnet session, so the terminal in charge is looked at after it. */
    com_flush();
    tio_start(length, manifold_term_in_charge());
}

void tio_read_start_unasked(void)
{
    tio_start(0, 0);
}

bool tio_ready(void)
{
    if (tio_state != TIO_READY)
    {
        bool answered = tio_answered();
        if ((answered && timer_mach_passed(tio_quiet)) ||
            timer_mach_passed(tio_deadline))
            tio_set_ready(answered);
    }
    return tio_state == TIO_READY;
}

void tio_ready_now(void)
{
    tio_set_ready(false);
}

void tio_line_end(void)
{
    if (tio_state != TIO_WAIT)
        return;
    bool answers = tio_charge == COM_TERM_SCREEN ||
                   (tio_charge == COM_TERM_TEL &&
                    ((tio_answers & COM_TERM_TEL) || !(tio_greeted & COM_TERM_TEL)));
    if (answers)
        tio_state = TIO_HELD;
    else
        tio_set_ready(false);
}

uint16_t HOST_IN_RAM_FUNC tio_prompt_col(void)
{
    return tio_state == TIO_READY ? tio_pcol : 0;
}

uint16_t HOST_IN_RAM_FUNC tio_get_term_width(void)
{
    if (tio_pinned_width)
        return tio_pinned_width;
    if (tio_naws_width)
        return tio_naws_width;
    if (manifold_term_in_charge() == COM_TERM_SCREEN)
    {
        int w, h;
        vga_canvas_size(&w, &h);
        return (uint16_t)vga_term_columns(w);
    }
    if (tio_prompt_col() && tio_width)
        return tio_width;
    return 80;
}

uint16_t tio_get_term_height(void)
{
    if (tio_pinned_height)
        return tio_pinned_height;
    if (tio_naws_height)
        return tio_naws_height;
    if (manifold_term_in_charge() == COM_TERM_SCREEN)
    {
        int w, h;
        vga_canvas_size(&w, &h);
        return (uint16_t)(h / vga_term_row_px(w));
    }
    if (tio_prompt_col() && tio_height)
        return tio_height;
    return 24;
}

void tio_set_term_width(uint16_t v) { tio_pinned_width = v; }
void tio_set_term_height(uint16_t v) { tio_pinned_height = v; }

void HOST_IN_RAM_FUNC tio_set_naws_size(uint16_t w, uint16_t h)
{
    tio_naws_width = w;
    tio_naws_height = h;
}

/* The read becomes ready first, so nothing marks the terminal again once it
 * is forgotten. */
void HOST_IN_RAM_FUNC tio_forget(com_source_t src)
{
    if (tio_charge == COM_TERM(src) && tio_state != TIO_READY)
        tio_set_ready(false);
    uint8_t keep = (uint8_t)~COM_TERM(src);
    memset(&tio_sources[src], 0, sizeof tio_sources[src]);
    tio_marks &= keep;
    tio_answers &= keep;
    tio_greeted &= keep;
}

void tio_cursor_shape(uint8_t ps)
{
    tio_shape = ps;
    tio_write_shape(tio_shaped(), ps);
}

void tio_cursor_default(void)
{
    tio_write_shape(tio_marks & manifold_terms_connected(), 0);
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
}

void tio_init(void)
{
    memset(tio_sources, 0, sizeof tio_sources);
    tio_state = TIO_READY;
    tio_charge = 0;
    tio_pcol = 0;
    tio_pinned_width = 0;
    tio_pinned_height = 0;
    tio_naws_width = 0;
    tio_naws_height = 0;
    tio_marks = COM_TERM_SCREEN;
    tio_answers = 0;
    tio_greeted = 0;
}

void tio_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    for (com_source_t s = COM_SOURCE_KEYBOARD; s < COM_SOURCE_COUNT; s++)
    {
        sst_put_u8(c, tio_sources[s].overlong);
        sst_put_u8(c, tio_sources[s].answered);
        sst_put_bool(c, tio_sources[s].da2);
        sst_put_u8(c, tio_sources[s].owed);
        sst_put_u8(c, tio_sources[s].open);
        sst_put_u64(c, tio_sources[s].quiet);
    }
    sst_put_u8(c, (uint8_t)tio_state);
    sst_put_u64(c, tio_deadline);
    sst_put_u64(c, tio_quiet);
    sst_put_u8(c, tio_charge);
    sst_put_bool(c, tio_probe);
    sst_put_bool(c, tio_sized);
    sst_put_u16(c, tio_pcol);
    sst_put_u16(c, tio_width);
    sst_put_u16(c, tio_height);
    sst_put_u16(c, tio_pinned_width);
    sst_put_u16(c, tio_pinned_height);
    sst_put_u16(c, tio_naws_width);
    sst_put_u16(c, tio_naws_height);
    sst_put_u8(c, tio_shape);
    sst_put_u8(c, tio_marks);
    sst_put_u8(c, tio_answers);
    sst_put_u8(c, tio_greeted);
}

bool tio_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    tio_source_t sources[COM_SOURCE_COUNT];
    for (com_source_t s = COM_SOURCE_KEYBOARD; s < COM_SOURCE_COUNT; s++)
    {
        sources[s].overlong = sst_get_u8(c);
        sources[s].answered = sst_get_u8(c);
        sources[s].da2 = sst_get_bool(c);
        sources[s].owed = sst_get_u8(c);
        sources[s].open = sst_get_u8(c);
        sources[s].quiet = sst_get_u64(c);
    }
    uint8_t state = sst_get_u8(c);
    uint64_t deadline = sst_get_u64(c), quiet = sst_get_u64(c);
    uint8_t charge = sst_get_u8(c);
    bool probe = sst_get_bool(c), sized = sst_get_bool(c);
    uint16_t pcol = sst_get_u16(c), width = sst_get_u16(c), height = sst_get_u16(c);
    uint16_t pin_w = sst_get_u16(c), pin_h = sst_get_u16(c);
    uint16_t naws_w = sst_get_u16(c), naws_h = sst_get_u16(c);
    uint8_t shape = sst_get_u8(c), marks = sst_get_u8(c);
    uint8_t answers = sst_get_u8(c), greeted = sst_get_u8(c);
    if (!sst_ok(c) || state > TIO_READY || (charge & (charge - 1)) ||
        (charge & (uint8_t)~(COM_TERM_ALL | COM_TERM_SCREEN)))
        return false;
    for (com_source_t s = COM_SOURCE_KEYBOARD; s < COM_SOURCE_COUNT; s++)
        if (sources[s].overlong > TIO_FRAME_LAST)
            return false;
    memcpy(tio_sources, sources, sizeof tio_sources);
    tio_state = (tio_state_t)state;
    tio_deadline = deadline;
    tio_quiet = quiet;
    tio_charge = charge;
    tio_probe = probe;
    tio_sized = sized;
    tio_pcol = pcol;
    tio_width = width;
    tio_height = height;
    tio_pinned_width = pin_w;
    tio_pinned_height = pin_h;
    tio_naws_width = naws_w;
    tio_naws_height = naws_h;
    tio_shape = shape;
    tio_marks = marks;
    tio_answers = answers;
    tio_greeted = greeted;
    return true;
}
