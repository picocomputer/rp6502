/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* tio over sources this file stands in for, each queue holding the number of
 * bytes a test gives it, and one byte that can be staged ahead of a source as
 * the 6502's receive register holds it. The terminals, the console's output,
 * the machine clock and the VGA are stood in for too. */

#include "core/com/manifold.h"
#include "core/com/tio.h"
#include "core/sys/timer.h"
#include "core/vga/vga.h"
#include "utest.h"
#include <string.h>

static struct
{
    char q[256];
    size_t len;
    size_t size;
} fake[COM_SOURCE_COUNT];

static com_source_t fake_staged_src = COM_SOURCE_ANY;
static char fake_staged;

/* Bytes past the queue's size are dropped, as a full ring drops them. */
static void push(com_source_t src, const char *s)
{
    for (; *s && fake[src].len < fake[src].size; s++)
        fake[src].q[fake[src].len++] = *s;
}

static void stage(com_source_t src, char c)
{
    fake_staged_src = src;
    fake_staged = c;
}

/* What was written to each terminal, whether the console was flushed before
 * the first write to one, and whether the next flush ends the telnet session. */
static char sent[COM_SOURCE_COUNT][256];
static bool flushed;
static bool wrote_unflushed;
static bool flush_ends_telnet;
static uint8_t fake_charge;
static uint8_t fake_connected;
static uint64_t fake_now;

static void sources(size_t keyboard, size_t uart)
{
    memset(fake, 0, sizeof fake);
    fake[COM_SOURCE_KEYBOARD].size = keyboard;
    fake[COM_SOURCE_UART].size = uart;
    fake[COM_SOURCE_TEL].size = 32;
    fake_staged_src = COM_SOURCE_ANY;
    memset(sent, 0, sizeof sent);
    flushed = false;
    wrote_unflushed = false;
    flush_ends_telnet = false;
    fake_charge = COM_TERM_UART;
    fake_connected = COM_TERM_UART;
    tio_init();
}

static void later_ms(uint64_t ms)
{
    fake_now += ms * 1000;
}

timer_mach_t timer_mach_in_ms(uint64_t ms)
{
    return fake_now + ms * 1000;
}

bool timer_mach_passed(timer_mach_t d)
{
    return (int64_t)(fake_now - d) >= 0;
}

void com_flush(void)
{
    flushed = true;
    if (flush_ends_telnet)
    {
        flush_ends_telnet = false;
        fake_charge = COM_TERM_UART;
        fake_connected = COM_TERM_UART;
        tio_forget(COM_SOURCE_TEL);
    }
}

void manifold_write_to(uint8_t to, const char *buf, size_t n)
{
    wrote_unflushed |= !flushed;
    for (com_source_t s = COM_SOURCE_UART; s < COM_SOURCE_COUNT; s++)
        if (to & COM_TERM(s))
            strncat(sent[s], buf, n);
}

uint8_t manifold_term_in_charge(void)
{
    return fake_charge;
}

uint8_t manifold_terms_connected(void)
{
    return fake_connected;
}

bool vga_connected(void)
{
    return false;
}

void vga_canvas_size(int *w, int *h)
{
    *w = 640;
    *h = 480;
}

size_t com_read(com_source_t src, char *buf, size_t n)
{
    size_t got = 0;
    if (n && fake_staged_src == src)
    {
        buf[got++] = fake_staged;
        fake_staged_src = COM_SOURCE_ANY;
    }
    size_t take = fake[src].len < n - got ? fake[src].len : n - got;
    memcpy(&buf[got], fake[src].q, take);
    memmove(fake[src].q, &fake[src].q[take], fake[src].len - take);
    fake[src].len -= take;
    return got + take;
}

int com_rx_peek(com_source_t src)
{
    return fake_staged_src == src ? (unsigned char)fake_staged : -1;
}

int com_peek(com_source_t src, size_t offset)
{
    if (fake_staged_src == src)
    {
        if (!offset)
            return (unsigned char)fake_staged;
        offset--;
    }
    return offset < fake[src].len ? (unsigned char)fake[src].q[offset] : -1;
}

size_t com_queue_size(com_source_t src)
{
    return fake[src].size;
}

static char got[TIO_ITEM_MAX + 1];
static com_source_t got_src;

/* The next item as a string, empty when there is none. */
static const char *next(void)
{
    size_t n = tio_read(&got_src, got);
    got[n] = 0;
    return got;
}

static char seq[300];

/* ESC '[', count digits, then the tail. */
static const char *csi_of(size_t count, const char *tail)
{
    strcpy(seq, "\33[");
    memset(seq + 2, '1', count);
    strcpy(seq + 2 + count, tail);
    return seq;
}

UTEST(tio, each_item_ends_by_the_framing_rules)
{
    sources(256, 256);
    push(COM_SOURCE_UART, "a\33[1;5C\33OA\33Nx\33b\33\33\33[?25h\33[ q");
    ASSERT_STREQ(next(), "a");
    ASSERT_EQ(got_src, COM_SOURCE_UART);
    ASSERT_STREQ(next(), "\33[1;5C");
    ASSERT_STREQ(next(), "\33OA");
    ASSERT_STREQ(next(), "\33Nx");
    ASSERT_STREQ(next(), "\33b");
    ASSERT_STREQ(next(), "\33\33");
    ASSERT_STREQ(next(), "\33[?25h");
    ASSERT_STREQ(next(), "\33[ ");
    ASSERT_STREQ(next(), "q");
    ASSERT_STREQ(next(), "");
}

UTEST(tio, a_can_drops_the_sequence_it_lands_in)
{
    sources(256, 256);
    push(COM_SOURCE_UART, "\33[12\30a\30b");
    ASSERT_STREQ(next(), "a");
    ASSERT_STREQ(next(), "b");
    ASSERT_STREQ(next(), "");
}

UTEST(tio, a_sequence_still_arriving_stays_in_its_source)
{
    sources(256, 256);
    push(COM_SOURCE_UART, "\33[1;");
    ASSERT_STREQ(next(), "");
    ASSERT_EQ(com_peek(COM_SOURCE_UART, 3), ';');
    push(COM_SOURCE_UART, "5C");
    ASSERT_STREQ(next(), "\33[1;5C");
}

UTEST(tio, the_keyboard_goes_first_and_a_waiting_esc_blocks_no_other_source)
{
    sources(256, 256);
    push(COM_SOURCE_UART, "u");
    push(COM_SOURCE_KEYBOARD, "k\33");
    ASSERT_STREQ(next(), "k");
    ASSERT_EQ(got_src, COM_SOURCE_KEYBOARD);
    ASSERT_STREQ(next(), "u");
    ASSERT_EQ(got_src, COM_SOURCE_UART);
    ASSERT_STREQ(next(), "");
}

UTEST(tio, an_item_of_32_bytes_is_whole)
{
    sources(256, 256);
    push(COM_SOURCE_UART, csi_of(29, "C"));
    ASSERT_EQ(strlen(next()), (size_t)32);
}

UTEST(tio, a_sequence_open_at_32_bytes_is_dropped_to_its_end)
{
    sources(256, 256);
    push(COM_SOURCE_UART, csi_of(30, ""));
    ASSERT_STREQ(next(), "");
    ASSERT_EQ(com_peek(COM_SOURCE_UART, 0), -1);
    push(COM_SOURCE_UART, csi_of(0, ";5Cx") + 2);
    ASSERT_STREQ(next(), "x");
    push(COM_SOURCE_UART, csi_of(100, "Cy"));
    ASSERT_STREQ(next(), "y");
}

UTEST(tio, a_16_byte_queue_ends_a_sequence_at_16)
{
    sources(16, 256);
    push(COM_SOURCE_KEYBOARD, csi_of(13, "C"));
    ASSERT_EQ(strlen(next()), (size_t)16);
    push(COM_SOURCE_KEYBOARD, csi_of(20, ""));
    ASSERT_EQ(fake[COM_SOURCE_KEYBOARD].len, (size_t)16);
    ASSERT_STREQ(next(), "");
    ASSERT_EQ(fake[COM_SOURCE_KEYBOARD].len, (size_t)0);
    push(COM_SOURCE_KEYBOARD, "12Ck");
    ASSERT_STREQ(next(), "k");
    ASSERT_EQ(got_src, COM_SOURCE_KEYBOARD);
}

UTEST(tio, a_staged_byte_counts_toward_a_16_byte_queue)
{
    sources(16, 256);
    stage(COM_SOURCE_KEYBOARD, '\33');
    push(COM_SOURCE_KEYBOARD, csi_of(14, "C") + 1);
    ASSERT_EQ(strlen(next()), (size_t)17);
    stage(COM_SOURCE_KEYBOARD, '\33');
    push(COM_SOURCE_KEYBOARD, csi_of(20, "") + 1);
    ASSERT_STREQ(next(), "");
    ASSERT_EQ(com_peek(COM_SOURCE_KEYBOARD, 0), -1);
    push(COM_SOURCE_KEYBOARD, "1Ck");
    ASSERT_STREQ(next(), "k");
}

UTEST(tio, a_staged_byte_from_another_source_does_not_count)
{
    sources(16, 256);
    stage(COM_SOURCE_UART, 'u');
    push(COM_SOURCE_KEYBOARD, csi_of(20, ""));
    ASSERT_STREQ(next(), "u");
    ASSERT_STREQ(next(), "");
    ASSERT_EQ(fake[COM_SOURCE_KEYBOARD].len, (size_t)0);
}

UTEST(tio, idle_is_no_whole_item_waiting)
{
    sources(256, 256);
    ASSERT_TRUE(tio_idle());
    push(COM_SOURCE_UART, "\33");
    ASSERT_TRUE(tio_idle());
    push(COM_SOURCE_UART, "[");
    ASSERT_TRUE(tio_idle());
    push(COM_SOURCE_UART, "A");
    ASSERT_FALSE(tio_idle());
    ASSERT_STREQ(next(), "\33[A");
    push(COM_SOURCE_KEYBOARD, "\33[1\30");
    ASSERT_TRUE(tio_idle());
    push(COM_SOURCE_KEYBOARD, "x");
    ASSERT_FALSE(tio_idle());
}

UTEST(tio, an_overlong_sequence_is_no_item_for_idle)
{
    sources(256, 256);
    push(COM_SOURCE_UART, csi_of(40, ""));
    ASSERT_TRUE(tio_idle());
    push(COM_SOURCE_UART, "C");
    ASSERT_TRUE(tio_idle());
    push(COM_SOURCE_UART, "z");
    ASSERT_FALSE(tio_idle());
    ASSERT_STREQ(next(), "z");
    push(COM_SOURCE_UART, csi_of(40, ""));
    ASSERT_STREQ(next(), "");
    push(COM_SOURCE_UART, "1");
    ASSERT_TRUE(tio_idle());
    push(COM_SOURCE_UART, "Cz");
    ASSERT_FALSE(tio_idle());
}

UTEST(tio, a_read_end_drops_the_rest_of_an_overlong_sequence_and_forgets_it)
{
    sources(256, 256);
    push(COM_SOURCE_UART, csi_of(40, ""));
    ASSERT_STREQ(next(), "");
    push(COM_SOURCE_UART, "1;1");
    tio_read_end();
    ASSERT_EQ(com_peek(COM_SOURCE_UART, 0), -1);
    push(COM_SOURCE_UART, "2C");
    ASSERT_STREQ(next(), "2");
    ASSERT_STREQ(next(), "C");
}

UTEST(tio, a_read_end_leaves_a_sequence_still_arriving)
{
    sources(256, 256);
    push(COM_SOURCE_UART, "\33[6;");
    tio_read_end();
    push(COM_SOURCE_UART, "3C");
    ASSERT_STREQ(next(), "\33[6;3C");
}

#define HIDE "\33[?25l"
#define SHOW "\33[?25h"
#define WRAP "\33[?7h"
#define SAVE "\0337"
#define CPR "\33[6n"
#define PROBE "\33[>c\0338 \b"
#define SIZE "\33[999;999H" CPR "\0338"

UTEST(tio, a_read_asks_the_terminal_in_charge_and_the_others_after_a_flush)
{
    sources(256, 256);
    fake_connected = COM_TERM_UART | COM_TERM_TEL;
    fake_charge = COM_TERM_TEL;
    tio_read_start(10);
    ASSERT_FALSE(wrote_unflushed);
    ASSERT_STREQ(sent[COM_SOURCE_TEL], HIDE WRAP SAVE CPR PROBE SIZE SHOW);
    ASSERT_STREQ(sent[COM_SOURCE_UART], HIDE WRAP SAVE PROBE CPR SHOW);
}

UTEST(tio, a_read_of_no_cells_asks_for_no_shapes_and_pinned_sizes_for_no_size)
{
    sources(256, 256);
    fake_connected = COM_TERM_UART | COM_TERM_TEL;
    tio_set_term_width(80);
    tio_set_term_height(24);
    tio_read_start(0);
    ASSERT_STREQ(sent[COM_SOURCE_UART], HIDE WRAP SAVE CPR SHOW);
    ASSERT_STREQ(sent[COM_SOURCE_TEL], WRAP);
}

UTEST(tio, a_terminal_not_connected_is_not_asked)
{
    sources(256, 256);
    tio_read_start(10);
    ASSERT_STREQ(sent[COM_SOURCE_UART], HIDE WRAP SAVE CPR PROBE SIZE SHOW);
    ASSERT_STREQ(sent[COM_SOURCE_TEL], "");
}

UTEST(tio, geometry_is_ready_once_the_answers_have_been_quiet)
{
    sources(256, 256);
    tio_read_start(10);
    push(COM_SOURCE_UART, "\33[5;3R\33[>1;10;0c\33[24;80R");
    ASSERT_STREQ(next(), "");
    ASSERT_FALSE(tio_ready());
    later_ms(TIO_QUIET_MS - 1);
    push(COM_SOURCE_KEYBOARD, "x");
    ASSERT_STREQ(next(), "x");
    later_ms(TIO_QUIET_MS - 1);
    ASSERT_FALSE(tio_ready());
    ASSERT_EQ(tio_prompt_col(), 0);
    later_ms(1);
    ASSERT_TRUE(tio_ready());
    ASSERT_EQ(tio_prompt_col(), 3);
    ASSERT_EQ(tio_get_term_width(), 80);
    ASSERT_EQ(tio_get_term_height(), 24);
}

UTEST(tio, an_answer_an_earlier_read_was_owed_does_not_win)
{
    sources(256, 256);
    tio_read_start(10);
    push(COM_SOURCE_UART, "\33[5;3R\33[>1;10;0c\33[24;80R");
    push(COM_SOURCE_UART, "\33[6;28R\33[>1;10;0c\33[24;80R");
    ASSERT_STREQ(next(), "");
    later_ms(TIO_QUIET_MS);
    ASSERT_TRUE(tio_ready());
    ASSERT_EQ(tio_prompt_col(), 28);
}

UTEST(tio, with_no_answer_the_deadline_brings_no_prompt_column)
{
    sources(256, 256);
    tio_read_start(10);
    later_ms(TIO_DEADLINE_MS - 1);
    ASSERT_FALSE(tio_ready());
    later_ms(1);
    ASSERT_TRUE(tio_ready());
    ASSERT_EQ(tio_prompt_col(), 0);
    ASSERT_EQ(tio_get_term_width(), 80);
    ASSERT_EQ(tio_get_term_height(), 24);
}

UTEST(tio, only_the_terminal_in_charge_gives_the_geometry)
{
    sources(256, 256);
    fake_connected = COM_TERM_UART | COM_TERM_TEL;
    fake_charge = COM_TERM_TEL;
    tio_read_start(10);
    push(COM_SOURCE_UART, "\33[>1;10;0c\33[5;3R\33[24;132R");
    ASSERT_STREQ(next(), "");
    later_ms(TIO_QUIET_MS);
    ASSERT_FALSE(tio_ready());
    push(COM_SOURCE_TEL, "\33[2;7R\33[>1;10;0c\33[30;100R");
    ASSERT_STREQ(next(), "");
    later_ms(TIO_QUIET_MS);
    ASSERT_TRUE(tio_ready());
    ASSERT_EQ(tio_prompt_col(), 7);
    ASSERT_EQ(tio_get_term_width(), 100);
    ASSERT_EQ(tio_get_term_height(), 30);
}

UTEST(tio, a_session_ended_by_the_flush_leaves_the_uart_in_charge)
{
    sources(256, 256);
    fake_connected = COM_TERM_UART | COM_TERM_TEL;
    fake_charge = COM_TERM_TEL;
    tio_read_start(10);
    tio_read_end();
    memset(sent, 0, sizeof sent);
    flush_ends_telnet = true;
    tio_read_start(10);
    ASSERT_STREQ(sent[COM_SOURCE_UART], HIDE WRAP SAVE CPR PROBE SIZE SHOW);
    ASSERT_STREQ(sent[COM_SOURCE_TEL], "");
    push(COM_SOURCE_UART, "\33[2;7R\33[>1;10;0c\33[30;100R");
    ASSERT_STREQ(next(), "");
    later_ms(TIO_QUIET_MS);
    ASSERT_TRUE(tio_ready());
    ASSERT_EQ(tio_prompt_col(), 7);
    ASSERT_EQ(tio_get_term_width(), 100);
}

UTEST(tio, a_later_answer_moves_the_prompt_column)
{
    sources(256, 256);
    tio_read_start(10);
    push(COM_SOURCE_UART, "\33[5;3R\33[>1;10;0c\33[24;80R");
    ASSERT_STREQ(next(), "");
    later_ms(TIO_QUIET_MS);
    ASSERT_TRUE(tio_ready());
    push(COM_SOURCE_UART, "\33[6;9R");
    ASSERT_STREQ(next(), "");
    ASSERT_EQ(tio_prompt_col(), 9);
}

UTEST(tio, ready_now_has_no_prompt_column_and_later_answers_change_nothing)
{
    sources(256, 256);
    tio_read_start(10);
    tio_ready_now();
    ASSERT_TRUE(tio_ready());
    push(COM_SOURCE_UART, "\33[5;3R\33[>1;10;0c\33[24;132R");
    ASSERT_STREQ(next(), "");
    ASSERT_EQ(tio_prompt_col(), 0);
    ASSERT_EQ(tio_get_term_width(), 80);
}

UTEST(tio, an_answer_from_the_keyboard_is_dropped)
{
    sources(256, 256);
    tio_read_start(10);
    push(COM_SOURCE_KEYBOARD, "\33[5;3R\33[>1c\33[24;80Rk");
    ASSERT_STREQ(next(), "k");
    later_ms(TIO_QUIET_MS);
    ASSERT_FALSE(tio_ready());
}

UTEST(tio, the_size_goes_pinned_then_naws_then_the_terminal)
{
    sources(256, 256);
    tio_read_start(10);
    push(COM_SOURCE_UART, "\33[5;3R\33[>1;10;0c\33[24;132R");
    ASSERT_STREQ(next(), "");
    later_ms(TIO_QUIET_MS);
    ASSERT_TRUE(tio_ready());
    ASSERT_EQ(tio_get_term_width(), 132);
    tio_set_naws_size(100, 40);
    ASSERT_EQ(tio_get_term_width(), 100);
    ASSERT_EQ(tio_get_term_height(), 40);
    tio_set_term_width(50);
    ASSERT_EQ(tio_get_term_width(), 50);
    ASSERT_EQ(tio_get_term_height(), 40);
}

UTEST(tio, a_stop_unpins_the_sizes)
{
    sources(256, 256);
    tio_set_term_width(50);
    tio_set_term_height(20);
    tio_stop();
    ASSERT_EQ(tio_get_term_width(), 80);
    ASSERT_EQ(tio_get_term_height(), 24);
}

/* Both terminals are connected, the UART in charge, and a read of 10 cells
 * starts with nothing sent yet. */
static void both_reading(void)
{
    fake_connected = COM_TERM_UART | COM_TERM_TEL;
    tio_read_start(10);
    memset(sent, 0, sizeof sent);
}

/* The answers are taken and the quiet dwell passes. */
static void answered(void)
{
    next();
    later_ms(TIO_QUIET_MS);
}

UTEST(tio, a_terminal_never_asked_gets_the_default_shape)
{
    sources(256, 256);
    fake_connected = COM_TERM_UART | COM_TERM_TEL;
    tio_cursor_default();
    ASSERT_STREQ(sent[COM_SOURCE_UART], "\33[0 q");
    ASSERT_STREQ(sent[COM_SOURCE_TEL], "\33[0 q");
}

UTEST(tio, a_terminal_that_answers_its_marker_and_not_da2_gets_no_shape)
{
    sources(256, 256);
    both_reading();
    push(COM_SOURCE_UART, "\33[5;3R\33[>1;10;0c\33[24;80R");
    push(COM_SOURCE_TEL, "\33[5;3R");
    answered();
    ASSERT_TRUE(tio_ready());
    tio_cursor_shape(5);
    ASSERT_STREQ(sent[COM_SOURCE_UART], "\33[5 q");
    ASSERT_STREQ(sent[COM_SOURCE_TEL], "");
    memset(sent, 0, sizeof sent);
    tio_cursor_default();
    ASSERT_STREQ(sent[COM_SOURCE_UART], "\33[0 q");
    ASSERT_STREQ(sent[COM_SOURCE_TEL], "");
}

UTEST(tio, a_da2_answer_takes_a_terminal_as_supporting_shapes_again)
{
    sources(256, 256);
    both_reading();
    push(COM_SOURCE_TEL, "\33[5;3R");
    ASSERT_STREQ(next(), "");
    tio_cursor_default();
    ASSERT_STREQ(sent[COM_SOURCE_TEL], "");
    both_reading();
    push(COM_SOURCE_TEL, "\33[>0;95;0c\33[5;3R");
    ASSERT_STREQ(next(), "");
    tio_cursor_default();
    ASSERT_STREQ(sent[COM_SOURCE_TEL], "\33[0 q");
}

UTEST(tio, a_read_of_no_cells_changes_no_marks)
{
    sources(256, 256);
    both_reading();
    push(COM_SOURCE_TEL, "\33[5;3R");
    ASSERT_STREQ(next(), "");
    tio_read_start(0);
    memset(sent, 0, sizeof sent);
    push(COM_SOURCE_UART, "\33[5;3R\33[24;80R");
    push(COM_SOURCE_TEL, "\33[>0;95;0c");
    answered();
    ASSERT_TRUE(tio_ready());
    tio_cursor_shape(5);
    ASSERT_STREQ(sent[COM_SOURCE_UART], "");
    ASSERT_STREQ(sent[COM_SOURCE_TEL], "");
    tio_cursor_default();
    ASSERT_STREQ(sent[COM_SOURCE_UART], "\33[0 q");
    ASSERT_STREQ(sent[COM_SOURCE_TEL], "");
}

UTEST(tio, with_both_sizes_pinned_a_cpr_and_no_da2_by_ready_marks_no_shapes)
{
    sources(256, 256);
    tio_set_term_width(80);
    tio_set_term_height(24);
    tio_read_start(10);
    push(COM_SOURCE_UART, "\33[5;3R");
    ASSERT_STREQ(next(), "");
    later_ms(TIO_QUIET_MS);
    ASSERT_TRUE(tio_ready());
    memset(sent, 0, sizeof sent);
    tio_cursor_default();
    ASSERT_STREQ(sent[COM_SOURCE_UART], "");
    tio_read_start(10);
    push(COM_SOURCE_UART, "\33[5;3R\33[>1;10;0c");
    ASSERT_STREQ(next(), "");
    later_ms(TIO_QUIET_MS);
    ASSERT_TRUE(tio_ready());
    memset(sent, 0, sizeof sent);
    tio_cursor_default();
    ASSERT_STREQ(sent[COM_SOURCE_UART], "\33[0 q");
}

UTEST(tio, a_da2_answer_after_the_edit_started_gets_the_current_shape)
{
    sources(256, 256);
    tio_read_start(10);
    push(COM_SOURCE_UART, "\33[5;3R");
    ASSERT_STREQ(next(), "");
    tio_ready_now();
    memset(sent, 0, sizeof sent);
    tio_cursor_shape(5);
    ASSERT_STREQ(sent[COM_SOURCE_UART], "");
    push(COM_SOURCE_UART, "\33[>1;10;0c");
    ASSERT_STREQ(next(), "");
    ASSERT_STREQ(sent[COM_SOURCE_UART], "\33[5 q");
    tio_cursor_shape(1);
    ASSERT_STREQ(sent[COM_SOURCE_UART], "\33[5 q\33[1 q");
}

/* The session on telnet is torn down with a sequence half taken and before it
 * answers, and the next session answers and types. */
UTEST(tio, a_telnet_session_replaced_during_a_read_gets_no_shape_and_loses_no_key)
{
    sources(256, 256);
    both_reading();
    push(COM_SOURCE_TEL, "\33[>0;95;0c");
    push(COM_SOURCE_TEL, csi_of(40, ""));
    ASSERT_STREQ(next(), "");
    fake[COM_SOURCE_TEL].len = 0;
    tio_forget(COM_SOURCE_TEL);
    push(COM_SOURCE_TEL, "\33[>0;95;0c\33[1;1Rk");
    ASSERT_STREQ(next(), "k");
    ASSERT_EQ(got_src, COM_SOURCE_TEL);
    push(COM_SOURCE_UART, "\33[5;3R\33[>1;10;0c\33[24;80R");
    answered();
    ASSERT_TRUE(tio_ready());
    tio_cursor_shape(5);
    ASSERT_STREQ(sent[COM_SOURCE_UART], "\33[5 q");
    ASSERT_STREQ(sent[COM_SOURCE_TEL], "");
}

/* A torn down session marked as without shapes leaves the next session
 * taken to support them. */
UTEST(tio, a_telnet_session_torn_down_takes_its_mark_with_it)
{
    sources(256, 256);
    both_reading();
    push(COM_SOURCE_TEL, "\33[5;3R");
    ASSERT_STREQ(next(), "");
    tio_forget(COM_SOURCE_TEL);
    tio_cursor_default();
    ASSERT_STREQ(sent[COM_SOURCE_TEL], "\33[0 q");
}

UTEST_MAIN();
