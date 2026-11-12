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

/* The framing states. TIO_FRAME_ONE waits for the one byte after ESC 'N' or
 * ESC 'O'. */
#define TIO_FRAME_GROUND 0
#define TIO_FRAME_ESC 1
#define TIO_FRAME_CSI 2
#define TIO_FRAME_ONE 3

/* The pieces of the questions a read asks. Save and restore bracket them so
 * the cursor ends where it started, and a space and a backspace clean up a
 * terminal that prints the 'c' of DA2. Autowrap goes on at every read of the
 * terminal in charge and stays on, because the line editor lets the terminal
 * wrap the line. */
#define TIO_HIDE "\33[?25l"
#define TIO_SHOW "\33[?25h"
#define TIO_WRAP "\33[?7h"
#define TIO_SAVE "\0337"
#define TIO_CPR "\33[6n"
#define TIO_PROBE "\33[>c\0338 \b"
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
    uint8_t overlong; // framing state inside an overlong sequence, 0 for none
    uint8_t answered; // CPRs answered this read
    bool da2;         // answered DA2 this read
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

tio_step_t HOST_IN_RAM_FUNC tio_frame(uint8_t *frame, uint8_t ch)
{
    uint8_t was = *frame;
    *frame = TIO_FRAME_GROUND;
    if (ch == TIO_CAN)
        return TIO_DROP;
    switch (was)
    {
    case TIO_FRAME_ESC:
        if (ch == '[')
            *frame = TIO_FRAME_CSI;
        else if (ch == 'N' || ch == 'O')
            *frame = TIO_FRAME_ONE;
        break;
    case TIO_FRAME_CSI:
        if ((ch >= '0' && ch <= '9') || ch == ';' || ch == ':' ||
            (ch >= '<' && ch <= '?'))
            *frame = TIO_FRAME_CSI;
        break;
    case TIO_FRAME_ONE:
        break;
    default:
        if (ch == TIO_ESC)
            *frame = TIO_FRAME_ESC;
        break;
    }
    return *frame == TIO_FRAME_GROUND ? TIO_END : TIO_OPEN;
}

bool HOST_IN_RAM_FUNC tio_csi_parse(const char *item, size_t len, tio_csi_t *csi)
{
    memset(csi, 0, sizeof *csi);
    if (len < 3 || item[1] != '[')
        return false;
    for (size_t i = 2; i < len - 1; i++)
    {
        uint8_t ch = (uint8_t)item[i];
        if (isdigit(ch))
        {
            if (csi->count < 2)
            {
                uint32_t v = (uint32_t)csi->param[csi->count] * 10 + (ch - '0');
                csi->param[csi->count] = v > UINT16_MAX ? UINT16_MAX : (uint16_t)v;
            }
        }
        else if (ch == ';' || ch == ':')
            csi->count++;
        else
            csi->priv = ch;
    }
    csi->count++;
    csi->final = (uint8_t)item[len - 1];
    return true;
}

/* Reads an answer to CPR or DA2 into csi. False for any other item. */
static bool HOST_IN_RAM_FUNC tio_is_answer(const char *item, size_t len, tio_csi_t *csi)
{
    return tio_csi_parse(item, len, csi) &&
           ((!csi->priv && csi->final == 'R' && csi->count == 2) ||
            (csi->priv == '>' && csi->final == 'c'));
}

/* How long a sequence at the front of src may grow: TIO_ITEM_MAX, or less
 * when the source's queue, with the byte staged from it, holds fewer. */
static size_t HOST_IN_RAM_FUNC tio_limit(com_source_t src)
{
    size_t limit = com_queue_size(src) + (com_rx_peek(src) >= 0);
    return limit < TIO_ITEM_MAX ? limit : TIO_ITEM_MAX;
}

/* Takes what has arrived of the overlong sequence at the front of src. True
 * once its end has been taken, or when src is in none. */
static bool HOST_IN_RAM_FUNC tio_skip(com_source_t src, char *scratch)
{
    while (tio_sources[src].overlong)
    {
        size_t n = 0;
        int c;
        while (tio_sources[src].overlong && n < TIO_ITEM_MAX &&
               (c = com_peek(src, n)) >= 0)
        {
            tio_frame(&tio_sources[src].overlong, (uint8_t)c);
            n++;
        }
        if (!n)
            return false;
        com_read(src, scratch, n);
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
 * is left where it is, unless it is overlong. While a line end is held, only
 * an answer is taken, and any other item at the front of the source the
 * terminal in charge answers on ends the wait, because an answer still to
 * come would arrive behind it. */
static size_t HOST_IN_RAM_FUNC tio_take(com_source_t src, char *buf)
{
    while (tio_skip(src, buf))
    {
        size_t limit = tio_limit(src);
        uint8_t frame = TIO_FRAME_GROUND;
        tio_step_t step;
        size_t n = 0;
        do
        {
            int c = com_peek(src, n);
            if (c < 0)
                return 0;
            step = tio_frame(&frame, (uint8_t)c);
            buf[n++] = (char)c;
        } while (step == TIO_OPEN && n < limit);
        tio_csi_t csi;
        if (tio_state == TIO_HELD &&
            (step != TIO_END || !tio_is_answer(buf, n, &csi)))
        {
            if (src == tio_charge_source())
                tio_set_ready(tio_answered());
            return 0;
        }
        com_read(src, buf, n);
        if (step == TIO_END)
            return n;
        /* A dropped sequence leaves the frame at 0, an overlong one in the
         * state its rest is skipped from. */
        tio_sources[src].overlong = frame;
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

/* Takes a terminal's answer to CPR or DA2. True when the item was one. */
static bool HOST_IN_RAM_FUNC tio_answer(com_source_t src, const char *item, size_t len)
{
    tio_csi_t csi;
    if (!tio_is_answer(item, len, &csi))
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
 * without taking anything. */
static bool HOST_IN_RAM_FUNC tio_waiting(com_source_t src)
{
    size_t limit = tio_limit(src);
    uint8_t frame = tio_sources[src].overlong;
    bool overlong = frame != TIO_FRAME_GROUND;
    size_t start = 0;
    for (size_t at = 0;; at++)
    {
        int c = com_peek(src, at);
        if (c < 0)
            return false;
        tio_step_t step = tio_frame(&frame, (uint8_t)c);
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
    char burst[sizeof(TIO_HIDE TIO_WRAP TIO_SAVE TIO_CPR TIO_PROBE TIO_SIZE TIO_SHOW)];
    size_t n = tio_cat(burst, 0, TIO_HIDE TIO_WRAP TIO_SAVE TIO_CPR);
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
    tio_marks = COM_TERM_UART | COM_TERM_SCREEN;
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
        if (sources[s].overlong > TIO_FRAME_ONE)
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
