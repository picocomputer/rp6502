/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Where a line ends. A terminal sends a return, a file of host text holds a
 * line feed, and a telnet client sends both for one Enter; the console
 * rewrites none of them, so the line editor is where all three are one rule.
 */

#include "core/com/com.h"
#include "core/str/rln.h"
#include "utest.h"
#include <string.h>

static int lines;
static char last[256];

/* A reader that asks for the next line, the way the monitor does. */
static void got_line(bool timeout, const char *buf)
{
    (void)timeout;
    lines++;
    strncpy(last, buf, sizeof last - 1);
    last[sizeof last - 1] = 0;
    rln_read_line(got_line);
}

/* What the wire delivered, read to the end of whatever it completes. */
static void wire(const char *s)
{
    com_uart_push(s, strlen(s));
    for (int i = 0; i < 64; i++)
        rln_task();
}

static void reading(void)
{
    lines = 0;
    last[0] = 0;
    rln_read_line(got_line);
}

UTEST(rln, either_spelling_ends_a_line)
{
    com_init();
    rln_init();
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
    com_init();
    rln_init();
    reading();
    wire("a\r\n");
    ASSERT_EQ(lines, 1);
    reading();
    wire("b\n\r");
    ASSERT_EQ(lines, 1);
}

UTEST(rln, the_memory_of_a_line_end_lasts_one_character)
{
    com_init();
    rln_init();
    /* Enter twice is a blank line, and a blank line in a file of CRLF text
     * is a line of its own. Both would go if the rule were "swallow the
     * next line feed" instead of "the opposite one, once". */
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
    com_init();
    rln_init();
    reading();
    wire("a\r");
    ASSERT_EQ(lines, 1);
    /* The monitor ran the line and the 6502 stopped, which walks every
     * driver's stop hook. What the source knows about the wire is not the
     * machine's to forget: the line feed still owed is still owed, and the
     * blank line it would otherwise make is not one the user typed. */
    rln_stop();
    reading();
    wire("\n");
    ASSERT_EQ(lines, 0);
    wire("b\r");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "b");
}

UTEST_MAIN();
