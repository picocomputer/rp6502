/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "host/sokol/cli/expr.h"
#include "utest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

UTEST_MAIN();

static const char *const expr_names[] = {
    "ok", "bad", "unknown", "ambiguous", "zero",
    "overflow", "shift", "float", "divide", "refused",
};

/* The same names as NAMES and REFUSED in expr_cases.py. */
static expr_status_t lookup(expr_form_t form, const char *name, size_t len,
                            int64_t *value)
{
    static const struct
    {
        expr_form_t form;
        const char *name;
        expr_status_t status;
        int64_t value;
    } names[] = {
        {EXPR_NAME, "a", EXPR_OK, 7},
        {EXPR_NAME, "b", EXPR_OK, -3},
        {EXPR_NAME, "p.q", EXPR_OK, 0x1234},
        {EXPR_NAME, "INT64_MAX", EXPR_OK, INT64_MAX},
        {EXPR_NAME, "INT64_MIN", EXPR_OK, INT64_MIN},
        {EXPR_ADDRESS, "a", EXPR_OK, 7},
        {EXPR_DEFINE, "b", EXPR_OK, -3},
        {EXPR_SIZEOF, "t", EXPR_OK, 12},
        {EXPR_COUNTOF, "t", EXPR_OK, 6},
        {EXPR_SIZEOF, "src/m.c/t", EXPR_OK, 12},
        {EXPR_COUNTOF, "src/m.c/t", EXPR_OK, 6},
        {EXPR_DEFINE, "src/m.c/p.q", EXPR_OK, 0x1234},
        {EXPR_ADDRESS, "b", EXPR_REFUSED, 0},
        {EXPR_DEFINE, "a", EXPR_REFUSED, 0},
        {EXPR_SIZEOF, "b", EXPR_REFUSED, 0},
        {EXPR_COUNTOF, "a", EXPR_REFUSED, 0},
    };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (names[i].form == form && strlen(names[i].name) == len &&
            !memcmp(names[i].name, name, len))
        {
            *value = names[i].value;
            return names[i].status;
        }
    return EXPR_UNKNOWN;
}

static bool same(const char *want, expr_status_t st, int64_t v)
{
    if (!strncmp(want, "int:", 4))
        return st == EXPR_OK && v == strtoll(want + 4, NULL, 10);
    return st != EXPR_OK && !strcmp(want, expr_names[st]);
}

UTEST(expr, matches_python)
{
    FILE *file = fopen(TEST_FIXTURE, "r");
    ASSERT_TRUE(file != NULL);
    char line[4096];
    int cases = 0, wrong = 0;
    while (fgets(line, sizeof line, file))
    {
        line[strcspn(line, "\r\n")] = 0;
        char *text = strchr(line, ' ');
        ASSERT_TRUE(text != NULL);
        *text++ = 0;
        int64_t v;
        expr_status_t st = expr_eval(text, strlen(text), lookup, &v);
        cases++;
        if (same(line, st, v))
            continue;
        wrong++;
        if (st != EXPR_OK)
            printf("%s: want %s, got %s\n", text, line, expr_names[st]);
        else
            printf("%s: want %s, got int:%lld\n", text, line, (long long)v);
    }
    fclose(file);
    EXPECT_EQ(wrong, 0);
    EXPECT_GE(cases, 20000);
}
