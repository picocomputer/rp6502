/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/api/api.h"
#include "core/com/com.h"
#include "core/com/manifold.h"
#include "core/ria/regs.h"
#include "core/str/rln.h"
#include "utest.h"
#include <string.h>

static int lines;
static char last[256];

static void got_line(bool timeout, const char *buf)
{
    (void)timeout;
    lines++;
    strncpy(last, buf, sizeof last - 1);
    last[sizeof last - 1] = 0;
    rln_read_line(got_line);
}

static void wire(const char *s)
{
    manifold_uart_push(s, strlen(s));
    for (int i = 0; i < 64; i++)
        rln_task();
}

/* rln_init keeps each source's line_end, so every source is also
 * cleared with rln_forget_source, or a case would start with the values the
 * previous case left. */
static void fresh(void)
{
    com_rx_clear();
    rln_init();
    for (com_source_t s = COM_SOURCE_KEYBOARD; s < COM_SOURCE_COUNT; s++)
        rln_forget_source(s);
}

static void reading(void)
{
    lines = 0;
    last[0] = 0;
    rln_read_line(got_line);
}

UTEST(rln, either_spelling_ends_a_line)
{
    fresh();
    reading();
    wire("a\r");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "a");
    reading();
    wire("b\n");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "b");
}

UTEST(rln, a_return_and_its_line_feed_are_one_line_end)
{
    fresh();
    reading();
    wire("a\r\n");
    ASSERT_EQ(lines, 1);
    reading();
    wire("b\n\r");
    ASSERT_EQ(lines, 1);
}

UTEST(rln, the_memory_of_a_line_end_lasts_one_character)
{
    fresh();
    reading();
    wire("\r\r");
    ASSERT_EQ(lines, 2);
    reading();
    wire("a\r\n\r\nb\r\n");
    ASSERT_EQ(lines, 3);
    ASSERT_STREQ(last, "b");
}

UTEST(rln, a_line_end_outlives_the_machine_it_was_typed_at)
{
    fresh();
    reading();
    wire("a\r");
    ASSERT_EQ(lines, 1);
    rln_stop();
    reading();
    wire("\n");
    ASSERT_EQ(lines, 0);
    wire("b\r");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "b");
}

UTEST(rln, a_source_that_goes_away_takes_its_half_line_end_with_it)
{
    fresh();
    reading();
    wire("a\r");
    ASSERT_EQ(lines, 1);
    rln_forget_source(COM_SOURCE_UART);
    reading();
    wire("\n");
    ASSERT_EQ(lines, 1);
}

/* Pulls the key and the action byte, as a program does. */
static int lastkey_len(void)
{
    rln_api_lastkey();
    int len = (int16_t)API_AX;
    if (len > 0)
        xstack_ptr += (size_t)len + 1;
    return len;
}

UTEST(rln, lastkey_and_peek_require_an_empty_xstack)
{
    fresh();
    reading();
    xstack_ptr = XSTACK_SIZE - 1;
    rln_api_lastkey();
    ASSERT_EQ((int16_t)API_AX, -1);
    xstack_ptr = XSTACK_SIZE - 1;
    rln_api_peek();
    ASSERT_EQ((int16_t)API_AX, -1);
    ASSERT_EQ(lastkey_len(), 0);
}

/* With both sizes fixed, one cursor position report starts the edit. */
static void editing(void)
{
    fresh();
    rln_set_term_width(80);
    rln_set_term_height(24);
    reading();
    wire("\33[1;1R");
}

UTEST(rln, a_key_longer_than_32_bytes_is_not_returned)
{
    editing();
    wire("\33[C");
    ASSERT_EQ(lastkey_len(), 3);
    char key[64] = "\33[";
    for (int i = 0; i < 16; i++)
        strcat(key, "1;");
    strcat(key, "C");
    wire(key);
    ASSERT_EQ(lastkey_len(), 0);
}

/* The Enter lands before the terminal answers the read's cursor position
 * request, so the up arrow after it is the next read's key. */
UTEST(rln, a_key_typed_right_after_enter_goes_to_the_next_read)
{
    editing();
    wire("x\r");
    ASSERT_EQ(lines, 1);
    wire("a\r\33[A");
    ASSERT_EQ(lines, 2);
    ASSERT_STREQ(last, "a");
    wire("\33[1;1R\r");
    ASSERT_EQ(lines, 3);
    ASSERT_STREQ(last, "a");
}

static char shown[4096];
static size_t shown_len;

static void show(const char *buf, int len)
{
    if (shown_len + (size_t)len < sizeof shown)
    {
        memcpy(shown + shown_len, buf, (size_t)len);
        shown_len += (size_t)len;
        shown[shown_len] = 0;
    }
}

static void capture(void)
{
    shown_len = 0;
    shown[0] = 0;
    manifold_set_tx_tap(show);
}

/* The monitor and std.c start the next read after the callback returns. */
static bool need_next;

static void got_line_later(bool timeout, const char *buf)
{
    (void)timeout;
    lines++;
    strncpy(last, buf, sizeof last - 1);
    last[sizeof last - 1] = 0;
    need_next = true;
}

static void next(void)
{
    if (need_next)
    {
        need_next = false;
        rln_read_line(got_line_later);
    }
}

/* A first read ends on an Enter typed before its terminal answered. Its
 * replies come during the second read, ahead of that read's own. The fields
 * are ten characters, so the cursor moves by absolute column. */
static void ended_early(void)
{
    fresh();
    rln_set_max_length(10);
    lines = 0;
    need_next = true;
    next();
    wire("a\r");
    next();
}

UTEST(rln, type_ahead_is_drawn_at_this_reads_prompt)
{
    ended_early();
    capture();
    wire("ab");
    wire("\33[5;4R\33[>1;10;0c\33[24;80R");
    wire("\33[6;28R\33[>1;10;0c\33[24;80R");
    manifold_set_tx_tap(NULL);
    ASSERT_TRUE(strstr(shown, "\33[28Ga") != NULL);
    ASSERT_TRUE(strstr(shown, "\33[4G") == NULL);
}

UTEST(rln, a_preload_is_drawn_at_this_reads_prompt)
{
    ended_early();
    rln_poke("ab");
    capture();
    wire("\33[5;28R\33[>1;10;0c\33[24;80R");
    wire("\33[6;4R\33[>1;10;0c\33[24;80R");
    manifold_set_tx_tap(NULL);
    ASSERT_TRUE(strstr(shown, "\33[4Ga") != NULL);
    ASSERT_TRUE(strstr(shown, "\33[28G") == NULL);
}

UTEST(rln, this_reads_size_wins)
{
    ended_early();
    wire("\33[5;3R\33[>1;10;0c\33[24;132R");
    wire("\33[6;28R\33[>1;10;0c\33[24;80R");
    ASSERT_EQ(rln_get_term_width(), 80);
}

/* The first read's Enter lands between its DA2 reply and its size reply. */
UTEST(rln, a_leftover_size_reply_keeps_the_cursor_shape)
{
    fresh();
    lines = 0;
    need_next = true;
    next();
    wire("\33[5;3R\33[>1;10;0c");
    wire("a\r");
    next();
    capture();
    wire("\33[24;80R");
    wire("\33[6;3R\33[>1;10;0c\33[24;80R");
    wire("\33[2~");
    manifold_set_tx_tap(NULL);
    ASSERT_TRUE(strstr(shown, "\33[5 q") != NULL);
    ASSERT_TRUE(strstr(shown, "\33[1 q") != NULL);
}

/* A poked Enter ends a read while its prompt reply is half way in. */
UTEST(rln, a_reply_cut_by_a_poked_enter_is_not_typed)
{
    fresh();
    lines = 0;
    need_next = true;
    next();
    wire("\33[5;3R\33[>1;10;0c\33[24;80R");
    rln_poke("\r");
    next();
    wire("\33[6;");
    rln_poke("\r");
    next();
    wire("3R\33[>1;10;0c\33[24;80R");
    wire("\33[7;3R\33[>1;10;0c\33[24;80R");
    wire("\r");
    ASSERT_EQ(lines, 3);
    ASSERT_STREQ(last, "");
}

/* A program stops while its read's prompt reply is half way in, and the
 * monitor reads. */
UTEST(rln, a_reply_cut_by_a_stop_is_not_typed)
{
    fresh();
    lines = 0;
    need_next = true;
    next();
    wire("\33[6;");
    rln_stop();
    need_next = true;
    next();
    wire("3R\33[>1;10;0c\33[24;80R");
    wire("\33[7;2R\33[>1;10;0c\33[24;80R");
    wire("\r");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "");
}

UTEST(rln, a_poke_of_an_empty_string_ends_the_line)
{
    editing();
    rln_poke("ab");
    ASSERT_EQ(lines, 0);
    rln_poke("");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "ab");
}

UTEST_MAIN();
