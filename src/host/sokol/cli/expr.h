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
} expr_status_t;

/* The lookup sets a name's value and returns EXPR_OK, or it returns
 * EXPR_UNKNOWN or EXPR_AMBIGUOUS. */
typedef expr_status_t (*expr_lookup_t)(const char *name, size_t len,
                                       int64_t *value);

/* When a name lookup fails, expr_eval sets name and name_len to that name. */
expr_status_t expr_eval(const char *text, size_t len, expr_lookup_t lookup,
                        int64_t *out, const char **name, size_t *name_len);

/* expr_int parses one whole-number literal with an optional leading -, for
 * reading .sym files. */
bool expr_int(const char *text, size_t len, int64_t *out);

#endif /* _HOST_SOKOL_CLI_EXPR_H_ */
