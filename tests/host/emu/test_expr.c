/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "host/sokol/cli/expr.h"
#include "utest.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

UTEST_MAIN();

static const char *const expr_names[] = {
    "ok", "bad", "unknown", "ambiguous", "zero",
    "overflow", "shift", "not_whole", "complex",
};

/* The same names as NAMES in expr_cases.py. */
static expr_status_t lookup(const char *name, size_t len, expr_value_t *value)
{
    static const struct
    {
        const char *name;
        expr_value_t value;
    } names[] = {
        {"a", {false, 7, 0}},
        {"b", {false, -3, 0}},
        {"f", {true, 0, 2.5}},
        {"p.q", {false, 0x1234, 0}},
        {"INT64_MAX", {false, INT64_MAX, 0}},
        {"INT64_MIN", {false, INT64_MIN, 0}},
    };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (strlen(names[i].name) == len && !memcmp(names[i].name, name, len))
        {
            *value = names[i].value;
            return EXPR_OK;
        }
    return EXPR_UNKNOWN;
}

/* A NaN answer, written float:nan, matches any NaN. */
static bool same(const char *want, expr_status_t st, const expr_value_t *v)
{
    if (!strncmp(want, "int:", 4))
        return st == EXPR_OK && !v->is_float &&
               v->i == strtoll(want + 4, NULL, 10);
    if (!strncmp(want, "float:", 6))
    {
        double f = strtod(want + 6, NULL);
        if (st != EXPR_OK || !v->is_float)
            return false;
        if (isnan(f))
            return isnan(v->f);
        return !memcmp(&f, &v->f, sizeof f);
    }
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
        expr_value_t v;
        const char *name;
        size_t name_len;
        expr_status_t st =
            expr_eval(text, strlen(text), lookup, &v, &name, &name_len);
        cases++;
        if (same(line, st, &v))
            continue;
        wrong++;
        if (st != EXPR_OK)
            printf("%s: want %s, got %s\n", text, line, expr_names[st]);
        else if (v.is_float)
            printf("%s: want %s, got float:%a\n", text, line, v.f);
        else
            printf("%s: want %s, got int:%lld\n", text, line, (long long)v.i);
    }
    fclose(file);
    EXPECT_EQ(wrong, 0);
    EXPECT_GE(cases, 20000);
}
