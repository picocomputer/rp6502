/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * core/sys/debug_log.h at the levels this test's CMake line set: WARN for
 * every category, DEBUG for the one named loud. host_log is this test's,
 * so the bench's stays out.
 */

#include "core/sys/debug_log.h"
#include "utest.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int calls;
static int last_level;
static char last_category[16];
static char last_text[8192];

void host_log(int level, const char *category, const char *fmt, ...)
{
    calls++;
    last_level = level;
    snprintf(last_category, sizeof last_category, "%s", category);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_text, sizeof last_text, fmt, ap);
    va_end(ap);
}

/* A call above the level evaluates nothing, arguments included. */
static int evaluated;

static int count(void)
{
    return ++evaluated;
}

UTEST(log, the_level_lets_through_what_is_at_or_below_it)
{
    calls = 0;
    RP6502_LOG(quiet, ERROR, "%s %d", "e", 1);
    ASSERT_EQ(calls, 1);
    ASSERT_EQ(last_level, RP6502_LOG_ERROR);
    ASSERT_STREQ(last_category, "quiet");
    ASSERT_STREQ(last_text, "e 1");
    RP6502_LOG(quiet, WARN, "w");
    ASSERT_EQ(calls, 2);
    ASSERT_EQ(last_level, RP6502_LOG_WARN);
    RP6502_LOG(quiet, INFO, "%d", count());
    RP6502_LOG(quiet, DEBUG, "%d", count());
    ASSERT_EQ(calls, 2);
    ASSERT_EQ(evaluated, 0);
}

UTEST(log, a_category_can_have_a_level_of_its_own)
{
    calls = 0;
    RP6502_LOG(loud, DEBUG, "d");
    ASSERT_EQ(calls, 1);
    ASSERT_EQ(last_level, RP6502_LOG_DEBUG);
    ASSERT_STREQ(last_category, "loud");
}

UTEST(log, a_message_is_a_printf_of_any_length)
{
    static char big[4097];
    memset(big, 'x', 4096);
    RP6502_LOG(quiet, ERROR, "%s %d %d %d %d %d %d %d %d %d %d %d %d",
               big, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12);
    ASSERT_EQ(strlen(last_text), 4096 + strlen(" 1 2 3 4 5 6 7 8 9 10 11 12"));
    ASSERT_STREQ(last_text + 4096, " 1 2 3 4 5 6 7 8 9 10 11 12");
}

UTEST_MAIN();
