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
 * terminal that prints the 'c' of DA2. Autowrap goes on for every read and
 * stays on, because the line editor lets the terminal wrap the line. */
#define TIO_HIDE "\33[?25l"
#define TIO_SHOW "\33[?25h"
#define TIO_WRAP "\33[?7h"
#define TIO_SAVE "\0337"
#define TIO_CPR "\33[6n"
#define TIO_PROBE "\33[>c\0338 \b"
#define TIO_SIZE "\33[999;999H" TIO_CPR "\0338"

typedef enum
{
    TIO_WAIT,
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
static com_source_t tio_charge; // the terminal in charge when the read started
static uint8_t tio_asked;       // COM_TERM_ bits of the terminals asked
static bool tio_probe;          // the read asked for cursor shapes
static bool tio_sized;          // the read asked the terminal in charge for the size

/* The latest prompt column, the largest column and the largest row the
 * terminal in charge reported. */
static uint16_t tio_pcol;
static uint16_t tio_width;
static uint16_t tio_height;

static uint16_t tio_pinned_width;
static uint16_t tio_pinned_height;
static uint16_t tio_naws_width;
static uint16_t tio_naws_height;

static uint8_t tio_shape; // the cursor shape the line editor asked for
static uint8_t tio_marks; // COM_TERM_ bits of terminals taken to do shapes

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

/* The next whole item at the front of src. A sequence that has not ended
 * is left where it is, unless it is overlong. */
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
            n++;
        } while (step == TIO_OPEN && n < limit);
        com_read(src, buf, n);
        if (step == TIO_END)
            return n;
        /* A dropped sequence leaves the frame at 0, an overlong one in the
         * state its rest is skipped from. */
        tio_sources[src].overlong = frame;
    }
    return 0;
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

/* How many CPRs a terminal was asked in this read: the terminal in charge
 * one for the prompt and, unless both sizes are pinned, one for the size;
 * every other terminal one, after DA2. */
static uint8_t HOST_IN_RAM_FUNC tio_due(com_source_t src)
{
    return src == tio_charge && tio_sized ? 2 : 1;
}

/* True when src was asked for cursor shapes in this read. */
static bool HOST_IN_RAM_FUNC tio_probed(com_source_t src)
{
    return tio_probe && (tio_asked & COM_TERM(src));
}

/* Writes DECSCUSR with the one-digit parameter ps to the terminals in to. */
static void tio_write_shape(uint8_t to, uint8_t ps)
{
    char seq[] = "\33[0 q";
    seq[2] = (char)('0' + ps);
    if (to)
        manifold_write_to(to, seq, sizeof seq - 1);
}

/* The answers are sorted by content, because an answer an earlier read was
 * owed comes ahead of this read's own. 999;999 clamps the size answer to the
 * bottom right, so it has the largest column, and an answer left of that is
 * a prompt answer, the latest one winning. With both sizes pinned every
 * answer is a prompt answer. Once geometry is ready without a prompt column,
 * the answers change nothing. */
static void HOST_IN_RAM_FUNC tio_cpr(com_source_t src, const tio_csi_t *csi)
{
    tio_source_t *t = &tio_sources[src];
    if (t->answered < UINT8_MAX)
        t->answered++;
    /* The marker is the first CPR asked after DA2: the size CPR of the
     * terminal in charge, the only CPR of the others. A terminal that does
     * DA2 answers it before the marker, so a marker answer with none before
     * it marks the terminal as without shapes. */
    if (tio_probed(src) && (src != tio_charge || tio_sized) &&
        t->answered == tio_due(src) && !t->da2)
        tio_marks &= (uint8_t)~COM_TERM(src);
    if (src != tio_charge || (tio_state == TIO_READY && !tio_pcol))
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

/* A DA2 answer to this read's question marks its terminal as supporting
 * shapes, and the line read sends that terminal its shapes. One that comes
 * after the edit started gets the current shape at once. */
static void HOST_IN_RAM_FUNC tio_da2(com_source_t src)
{
    if (!tio_probed(src))
        return;
    tio_marks |= COM_TERM(src);
    tio_sources[src].da2 = true;
    if (tio_state == TIO_READY)
        tio_write_shape(COM_TERM(src), tio_shape);
}

/* Takes a terminal's answer to CPR or DA2. True when the item was one. */
static bool HOST_IN_RAM_FUNC tio_answer(com_source_t src, const char *item, size_t len)
{
    tio_csi_t csi;
    if (!tio_csi_parse(item, len, &csi))
        return false;
    bool cpr = !csi.priv && csi.final == 'R' && csi.count == 2;
    bool da2 = csi.priv == '>' && csi.final == 'c';
    if (src != COM_SOURCE_KEYBOARD)
    {
        if (cpr)
            tio_cpr(src, &csi);
        if (da2)
            tio_da2(src);
    }
    return cpr || da2;
}

size_t HOST_IN_RAM_FUNC tio_read(com_source_t *src, char *buf)
{
    com_source_t s = COM_SOURCE_KEYBOARD;
    while (s < COM_SOURCE_COUNT)
    {
        size_t n = tio_take(s, buf);
        if (!n)
        {
            s++;
            continue;
        }
        if (tio_state == TIO_WAIT)
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

void tio_read_start(uint8_t length)
{
    /* A question longer than the room left in the console's queue would go
     * out ahead of the end of what a program printed. The flush can end a
     * telnet session, so the terminals are looked at after it. */
    com_flush();
    uint8_t charge = manifold_term_in_charge();
    tio_asked = manifold_terms_connected();
    tio_probe = length > 0;
    tio_sized = !(tio_pinned_width && tio_pinned_height);
    tio_state = TIO_WAIT;
    tio_deadline = timer_mach_in_ms(TIO_DEADLINE_MS);
    tio_quiet = tio_deadline;
    tio_pcol = 0;
    tio_width = 0;
    tio_height = 0;
    for (com_source_t s = COM_SOURCE_UART; s < COM_SOURCE_COUNT; s++)
    {
        tio_sources[s].answered = 0;
        tio_sources[s].da2 = false;
        uint8_t to = COM_TERM(s);
        if (to == charge)
            tio_charge = s;
        if (!(tio_asked & to))
            continue;
        char burst[sizeof(TIO_HIDE TIO_WRAP TIO_SAVE TIO_CPR TIO_PROBE TIO_SIZE TIO_SHOW)];
        size_t n = 0;
        if (to == charge)
        {
            n = tio_cat(burst, n, TIO_HIDE TIO_WRAP TIO_SAVE TIO_CPR);
            if (tio_probe)
                n = tio_cat(burst, n, TIO_PROBE);
            if (tio_sized)
                n = tio_cat(burst, n, TIO_SIZE);
            n = tio_cat(burst, n, TIO_SHOW);
        }
        else if (tio_probe)
            n = tio_cat(burst, n, TIO_HIDE TIO_WRAP TIO_SAVE TIO_PROBE TIO_CPR TIO_SHOW);
        else
            n = tio_cat(burst, n, TIO_WRAP);
        manifold_write_to(to, burst, n);
    }
}

/* Geometry becomes ready, keeping what the terminal in charge reported only
 * when it has answered. With both sizes pinned the terminal in charge is
 * asked no marker, so one that has answered its CPR and not DA2 by now is
 * marked as without shapes. */
static void tio_set_ready(bool answered)
{
    tio_source_t *t = &tio_sources[tio_charge];
    tio_state = TIO_READY;
    if (!answered)
        tio_pcol = 0;
    if (tio_probed(tio_charge) && !tio_sized && t->answered && !t->da2)
        tio_marks &= (uint8_t)~COM_TERM(tio_charge);
}

bool tio_ready(void)
{
    if (tio_state == TIO_WAIT)
    {
        bool answered = tio_sources[tio_charge].answered >= tio_due(tio_charge);
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
    if (tio_prompt_col() && tio_width)
        return tio_width;
    if (vga_connected())
    {
        int w, h;
        vga_canvas_size(&w, &h);
        return (uint16_t)vga_term_columns(w);
    }
    return 80;
}

uint16_t tio_get_term_height(void)
{
    if (tio_pinned_height)
        return tio_pinned_height;
    if (tio_naws_height)
        return tio_naws_height;
    if (tio_prompt_col() && tio_height)
        return tio_height;
    if (vga_connected())
    {
        int w, h;
        vga_canvas_size(&w, &h);
        return (uint16_t)(h / vga_term_row_px(w));
    }
    return 24;
}

void tio_set_term_width(uint16_t v) { tio_pinned_width = v; }
void tio_set_term_height(uint16_t v) { tio_pinned_height = v; }

void HOST_IN_RAM_FUNC tio_set_naws_size(uint16_t w, uint16_t h)
{
    tio_naws_width = w;
    tio_naws_height = h;
}

void HOST_IN_RAM_FUNC tio_forget(com_source_t src)
{
    memset(&tio_sources[src], 0, sizeof tio_sources[src]);
    tio_asked &= (uint8_t)~COM_TERM(src);
    tio_marks |= COM_TERM(src);
}

void tio_cursor_shape(uint8_t ps)
{
    uint8_t to = 0;
    for (com_source_t s = COM_SOURCE_UART; s < COM_SOURCE_COUNT; s++)
        if (tio_sources[s].da2)
            to |= COM_TERM(s);
    tio_shape = ps;
    tio_write_shape(to, ps);
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
    tio_charge = COM_SOURCE_UART;
    tio_pcol = 0;
    tio_pinned_width = 0;
    tio_pinned_height = 0;
    tio_naws_width = 0;
    tio_naws_height = 0;
    tio_marks = COM_TERM_ALL;
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
    sst_put_u8(c, (uint8_t)tio_charge);
    sst_put_u8(c, tio_asked);
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
    uint8_t charge = sst_get_u8(c), asked = sst_get_u8(c);
    bool probe = sst_get_bool(c), sized = sst_get_bool(c);
    uint16_t pcol = sst_get_u16(c), width = sst_get_u16(c), height = sst_get_u16(c);
    uint16_t pin_w = sst_get_u16(c), pin_h = sst_get_u16(c);
    uint16_t naws_w = sst_get_u16(c), naws_h = sst_get_u16(c);
    uint8_t shape = sst_get_u8(c), marks = sst_get_u8(c);
    if (!sst_ok(c) || state > TIO_READY || charge < COM_SOURCE_UART ||
        charge >= COM_SOURCE_COUNT)
        return false;
    for (com_source_t s = COM_SOURCE_KEYBOARD; s < COM_SOURCE_COUNT; s++)
        if (sources[s].overlong > TIO_FRAME_ONE)
            return false;
    memcpy(tio_sources, sources, sizeof tio_sources);
    tio_state = (tio_state_t)state;
    tio_deadline = deadline;
    tio_quiet = quiet;
    tio_charge = (com_source_t)charge;
    tio_asked = asked;
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
    return true;
}
