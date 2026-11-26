/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _HOST_SOKOL_CLI_EXPR_H_
#define _HOST_SOKOL_CLI_EXPR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* expr_eval evaluates script expressions with Python 3's rules for whole
 * numbers. Numbers are 64-bit, and a result that does not fit is an overflow
 * error. Floats are not allowed, so a float such as 1.5, a /, or a negative
 * power is an error. */

typedef enum
{
    EXPR_OK,
    EXPR_BAD,
    EXPR_UNKNOWN,
    EXPR_AMBIGUOUS,
    EXPR_ZERO,
    EXPR_OVERFLOW,
    EXPR_SHIFT,
    EXPR_FLOAT,
    EXPR_DIVIDE,
    EXPR_REFUSED,
} expr_status_t;

/* A name, or the text between the parentheses of address(), define(),
 * sizeof() or countof(). That text is not an expression, so it can hold a
 * path such as src/game.c/events. */
typedef enum
{
    EXPR_NAME,
    EXPR_ADDRESS,
    EXPR_DEFINE,
    EXPR_SIZEOF,
    EXPR_COUNTOF,
} expr_form_t;

/* The lookup sets the value and returns EXPR_OK, or it returns
 * EXPR_UNKNOWN, EXPR_AMBIGUOUS, or EXPR_REFUSED for a name that has no such
 * value. expr_eval returns what the lookup returns. */
typedef expr_status_t (*expr_lookup_t)(expr_form_t form, const char *name,
                                       size_t len, int64_t *value);

expr_status_t expr_eval(const char *text, size_t len, expr_lookup_t lookup,
                        int64_t *out);

/* expr_int parses one whole-number literal with an optional leading -, for
 * reading .sym files. */
bool expr_int(const char *text, size_t len, int64_t *out);

#endif /* _HOST_SOKOL_CLI_EXPR_H_ */
