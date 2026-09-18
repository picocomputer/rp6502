/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/com/com.h"
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
    com_uart_push(s, strlen(s));
    for (int i = 0; i < 64; i++)
        rln_task();
}

/* rln_init keeps each source's line_end and cpr_seen, so every source is also
 * cleared with rln_forget_source, or a case would start with the values the
 * previous case left. */
static void fresh(void)
{
    com_init();
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

UTEST_MAIN();
