/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "host/sokol/cli/expr.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef enum
{
    EXPR_T_END,
    EXPR_T_BAD,
    EXPR_T_NUM,
    EXPR_T_NAME,
    EXPR_T_OR,
    EXPR_T_AND,
    EXPR_T_NOT,
    EXPR_T_LT,
    EXPR_T_LE,
    EXPR_T_GT,
    EXPR_T_GE,
    EXPR_T_EQ,
    EXPR_T_NE,
    EXPR_T_BOR,
    EXPR_T_XOR,
    EXPR_T_BAND,
    EXPR_T_SHL,
    EXPR_T_SHR,
    EXPR_T_ADD,
    EXPR_T_SUB,
    EXPR_T_MUL,
    EXPR_T_DIV,
    EXPR_T_FLOORDIV,
    EXPR_T_MOD,
    EXPR_T_POW,
    EXPR_T_INV,
    EXPR_T_OPEN,
    EXPR_T_CLOSE,
} expr_tok_t;

/* The two-character operators are listed first so that ** matches before *. */
static const struct
{
    const char *text;
    expr_tok_t tok;
} expr_ops[] = {
    {"**", EXPR_T_POW},
    {"//", EXPR_T_FLOORDIV},
    {"<<", EXPR_T_SHL},
    {">>", EXPR_T_SHR},
    {"<=", EXPR_T_LE},
    {">=", EXPR_T_GE},
    {"==", EXPR_T_EQ},
    {"!=", EXPR_T_NE},
    {"<", EXPR_T_LT},
    {">", EXPR_T_GT},
    {"|", EXPR_T_BOR},
    {"^", EXPR_T_XOR},
    {"&", EXPR_T_BAND},
    {"+", EXPR_T_ADD},
    {"-", EXPR_T_SUB},
    {"*", EXPR_T_MUL},
    {"/", EXPR_T_DIV},
    {"%", EXPR_T_MOD},
    {"~", EXPR_T_INV},
    {"(", EXPR_T_OPEN},
    {")", EXPR_T_CLOSE},
};

typedef struct
{
    const char *s, *end;
    expr_tok_t tok;
    const char *text; /* the token, for numbers and names */
    size_t len;
    uint64_t whole;
    bool big; /* the whole number is past UINT64_MAX */
    bool is_float;
    bool skip; /* read the expression without working anything out */
    expr_lookup_t lookup;
    const char **name;
    size_t *name_len;
} expr_t;

static int expr_digit(char c, int base)
{
    int d = 99;
    if (c >= '0' && c <= '9')
        d = c - '0';
    else if (c >= 'a' && c <= 'z')
        d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z')
        d = c - 'A' + 10;
    return d < base ? d : -1;
}

static bool expr_name_char(char c)
{
    return expr_digit(c, 36) >= 0 || c == '_' || c == '.';
}

/* Reads digits with a _ allowed between two of them, and after a prefix when
 * lead is set. Returns the end, or NULL if they are not valid in Python. */
static const char *expr_digits(const char *s, const char *end, int base,
                               bool lead, uint64_t *value, bool *big)
{
    bool need = true; /* a digit must come next */
    if (lead && s < end && *s == '_')
        s++;
    for (; s < end; s++)
    {
        int d = expr_digit(*s, base);
        if (d < 0 && (need || *s != '_'))
            break;
        need = d < 0;
        if (d < 0)
            continue;
        if (*value > (UINT64_MAX - (uint64_t)d) / (uint64_t)base)
            *big = true;
        *value = *value * (uint64_t)base + (uint64_t)d;
    }
    return need ? NULL : s;
}

/* Reads a number in Python's syntax, or hex with a leading $. Returns the end,
 * or NULL if the number is invalid. strtod gives a float's value later. */
static const char *expr_number(const char *s, const char *end, uint64_t *whole,
                               bool *big, bool *is_float)
{
    *whole = 0, *big = false, *is_float = false;
    int base = 0;
    if (*s == '$')
        base = 16, s++;
    else if (*s == '0' && end - s > 1)
    {
        if (s[1] == 'x' || s[1] == 'X')
            base = 16;
        else if (s[1] == 'o' || s[1] == 'O')
            base = 8;
        else if (s[1] == 'b' || s[1] == 'B')
            base = 2;
        if (base)
            s += 2;
    }
    if (base)
        return expr_digits(s, end, base, true, whole, big);

    const char *start = s;
    uint64_t part = 0;
    bool part_big = false;
    if (*s != '.' && !(s = expr_digits(s, end, 10, false, whole, big)))
        return NULL;
    if (s < end && *s == '.')
    {
        *is_float = true;
        if (++s < end && expr_digit(*s, 10) >= 0 &&
            !(s = expr_digits(s, end, 10, false, &part, &part_big)))
            return NULL;
    }
    if (s < end && (*s == 'e' || *s == 'E'))
    {
        const char *t = s + 1;
        if (t < end && (*t == '+' || *t == '-'))
            t++;
        if (t < end && expr_digit(*t, 10) >= 0)
        {
            *is_float = true;
            s = expr_digits(t, end, 10, false, &part, &part_big);
        }
        else if (t > s + 1)
            return NULL;
    }
    /* In Python, 010 is a syntax error, but 00, 010.5 and 01e1 are valid. */
    if (s && !*is_float && *start == '0' && (*whole || *big))
        return NULL;
    return s;
}

static void expr_next(expr_t *e)
{
    const char *s = e->s;
    while (s < e->end && (*s == ' ' || *s == '\t'))
        s++;
    e->text = s;
    e->tok = EXPR_T_BAD;
    if (s == e->end)
        e->tok = EXPR_T_END;
    else if (expr_digit(*s, 10) >= 0 || *s == '$' ||
             (*s == '.' && s + 1 < e->end && expr_digit(s[1], 10) >= 0))
    {
        s = expr_number(s, e->end, &e->whole, &e->big, &e->is_float);
        if (!s)
            return;
        e->tok = EXPR_T_NUM;
    }
    else if (expr_digit(*s, 36) >= 10 || *s == '_')
    {
        /* A keyword ends at a ., as in Python's 1 and.5. */
        while (s < e->end && expr_name_char(*s) && *s != '.')
            s++;
        size_t len = (size_t)(s - e->text);
        e->tok = EXPR_T_NAME;
        if (len == 2 && !memcmp(e->text, "or", 2))
            e->tok = EXPR_T_OR;
        else if (len == 3 && !memcmp(e->text, "and", 3))
            e->tok = EXPR_T_AND;
        else if (len == 3 && !memcmp(e->text, "not", 3))
            e->tok = EXPR_T_NOT;
        else
            while (s < e->end && expr_name_char(*s))
                s++;
    }
    else
    {
        for (size_t i = 0; i < sizeof expr_ops / sizeof *expr_ops; i++)
        {
            size_t n = strlen(expr_ops[i].text);
            if ((size_t)(e->end - s) >= n && !memcmp(s, expr_ops[i].text, n))
            {
                e->tok = expr_ops[i].tok;
                s += n;
                break;
            }
        }
        if (e->tok == EXPR_T_BAD)
            return;
    }
    e->len = (size_t)(s - e->text);
    e->s = s;
}

static bool expr_true(const expr_value_t *v)
{
    return v->is_float ? v->f != 0.0 : v->i != 0;
}

static bool expr_mul(int64_t x, int64_t y, int64_t *out)
{
    if (x && y &&
        (x > 0 ? (y > 0 ? x > INT64_MAX / y : y < INT64_MIN / x)
               : (y > 0 ? x < INT64_MIN / y : x < INT64_MAX / y)))
        return false;
    *out = x * y;
    return true;
}

/* This function follows CPython's float_pow. For NaN and infinity, CPython
 * gives the same results as C's pow. */
static expr_status_t expr_pow(double x, double y, double *out)
{
    if (y == 0)
    {
        *out = 1.0;
        return EXPR_OK;
    }
    if (!isfinite(x) || !isfinite(y))
    {
        *out = pow(x, y);
        return EXPR_OK;
    }
    if (x == 0 && y < 0)
        return EXPR_ZERO;
    /* Python makes a complex number here, unless its size overflows. */
    if (x < 0 && y != floor(y))
        return isinf(pow(-x, y)) ? EXPR_OVERFLOW : EXPR_COMPLEX;
    *out = pow(x, y);
    return isinf(*out) ? EXPR_OVERFLOW : EXPR_OK;
}

static expr_status_t expr_floats(expr_tok_t op, double x, double y, double *out)
{
    switch (op)
    {
    case EXPR_T_ADD:
        *out = x + y;
        return EXPR_OK;
    case EXPR_T_SUB:
        *out = x - y;
        return EXPR_OK;
    case EXPR_T_MUL:
        *out = x * y;
        return EXPR_OK;
    case EXPR_T_DIV:
        if (y == 0)
            return EXPR_ZERO;
        *out = x / y;
        return EXPR_OK;
    case EXPR_T_FLOORDIV:
    case EXPR_T_MOD:
    {
        /* This code follows CPython's _float_div_mod exactly. */
        if (y == 0)
            return EXPR_ZERO;
        double mod = fmod(x, y);
        double div = (x - mod) / y;
        if (mod)
        {
            if ((y < 0) != (mod < 0))
            {
                mod += y;
                div -= 1.0;
            }
        }
        else
            mod = copysign(0.0, y);
        if (op == EXPR_T_MOD)
            *out = mod;
        else if (div)
        {
            *out = floor(div);
            if (div - *out > 0.5)
                *out += 1.0;
        }
        else
            *out = copysign(0.0, x / y);
        return EXPR_OK;
    }
    case EXPR_T_POW:
        return expr_pow(x, y, out);
    default:
        return EXPR_NOT_WHOLE;
    }
}

/* Returns x / y with a single rounding, as Python does. Above 2 ** 53,
 * converting x and y to doubles first would add a second rounding. */
static double expr_quotient(int64_t x, int64_t y)
{
    uint64_t a = x < 0 ? 0 - (uint64_t)x : (uint64_t)x;
    uint64_t b = y < 0 ? 0 - (uint64_t)y : (uint64_t)y;
    if (!a || (a <= 1ull << 53 && b <= 1ull << 53))
        return (double)x / (double)y;
    /* Work out at least 55 bits of the quotient and set the lowest bit if any
     * remainder is left, so that the conversion to double rounds correctly. */
    uint64_t q = a / b, r = a % b;
    int exp = 0;
    for (; q < 1ull << 54; exp--)
    {
        bool bit = r >= b - r;
        r = bit ? r - (b - r) : r * 2;
        q = q * 2 + bit;
    }
    double f = ldexp((double)(q | (r != 0)), exp);
    return (x < 0) != (y < 0) ? -f : f;
}

static expr_status_t expr_wholes(expr_tok_t op, expr_value_t *v, int64_t y)
{
    int64_t x = v->i;
    switch (op)
    {
    case EXPR_T_ADD:
        if (y > 0 ? x > INT64_MAX - y : x < INT64_MIN - y)
            return EXPR_OVERFLOW;
        v->i = x + y;
        return EXPR_OK;
    case EXPR_T_SUB:
        if (y < 0 ? x > INT64_MAX + y : x < INT64_MIN + y)
            return EXPR_OVERFLOW;
        v->i = x - y;
        return EXPR_OK;
    case EXPR_T_MUL:
        return expr_mul(x, y, &v->i) ? EXPR_OK : EXPR_OVERFLOW;
    case EXPR_T_DIV:
        if (!y)
            return EXPR_ZERO;
        v->is_float = true;
        v->f = expr_quotient(x, y);
        return EXPR_OK;
    case EXPR_T_FLOORDIV:
    case EXPR_T_MOD:
        if (!y)
            return EXPR_ZERO;
        if (y == -1)
        {
            /* C's INT64_MIN / -1 and INT64_MIN % -1 crash. */
            if (op == EXPR_T_MOD)
                v->i = 0;
            else if (x == INT64_MIN)
                return EXPR_OVERFLOW;
            else
                v->i = -x;
            return EXPR_OK;
        }
        {
            int64_t q = x / y, r = x % y;
            if (r && (r < 0) != (y < 0))
                q--, r += y;
            v->i = op == EXPR_T_MOD ? r : q;
        }
        return EXPR_OK;
    case EXPR_T_POW:
        if (y < 0)
        {
            v->is_float = true;
            return expr_pow((double)x, (double)y, &v->f);
        }
        {
            /* Squaring stops after the last bit, so (-2) ** 63 fits. */
            int64_t r = 1;
            for (;;)
            {
                if ((y & 1) && !expr_mul(r, x, &r))
                    return EXPR_OVERFLOW;
                y >>= 1;
                if (!y)
                    break;
                if (!expr_mul(x, x, &x))
                    return EXPR_OVERFLOW;
            }
            v->i = r;
        }
        return EXPR_OK;
    case EXPR_T_SHL:
        if (y < 0)
            return EXPR_SHIFT;
        if (!x)
            return EXPR_OK;
        if (y >= 64)
            return EXPR_OVERFLOW;
        /* The result overflows unless shifting it back gives x. */
        v->i = (int64_t)((uint64_t)x << y);
        return v->i >> y == x ? EXPR_OK : EXPR_OVERFLOW;
    case EXPR_T_SHR:
        if (y < 0)
            return EXPR_SHIFT;
        v->i = y >= 64 ? (x < 0 ? -1 : 0) : x >> y;
        return EXPR_OK;
    case EXPR_T_BAND:
        v->i = x & y;
        return EXPR_OK;
    case EXPR_T_BOR:
        v->i = x | y;
        return EXPR_OK;
    default:
        v->i = x ^ y;
        return EXPR_OK;
    }
}

static expr_status_t expr_binary(expr_tok_t op, expr_value_t *v,
                                 const expr_value_t *r)
{
    if (!v->is_float && !r->is_float)
        return expr_wholes(op, v, r->i);
    double x = v->is_float ? v->f : (double)v->i;
    double y = r->is_float ? r->f : (double)r->i;
    v->is_float = true;
    return expr_floats(op, x, y, &v->f);
}

/* Returns -1, 0 or 1 when x is below, equal to or above y, and 2 when either
 * is NaN. A whole number and a float compare exactly, without rounding. */
static int expr_order(const expr_value_t *x, const expr_value_t *y)
{
    if (x->is_float && y->is_float)
        return isnan(x->f) || isnan(y->f) ? 2 : (x->f > y->f) - (x->f < y->f);
    if (!x->is_float && !y->is_float)
        return (x->i > y->i) - (x->i < y->i);
    if (x->is_float)
    {
        int c = expr_order(y, x);
        return c == 2 ? 2 : -c;
    }
    double f = y->f;
    if (isnan(f))
        return 2;
    if (f >= 9223372036854775808.0)
        return -1;
    if (f < -9223372036854775808.0)
        return 1;
    /* f is inside the int64 range here, so its whole part converts exactly. */
    double t = trunc(f);
    int64_t whole = (int64_t)t;
    if (x->i != whole)
        return x->i < whole ? -1 : 1;
    return (t < f) ? -1 : (t > f);
}

static bool expr_holds(expr_tok_t op, const expr_value_t *x,
                       const expr_value_t *y)
{
    int c = expr_order(x, y);
    switch (op)
    {
    case EXPR_T_LT:
        return c == -1;
    case EXPR_T_LE:
        return c == -1 || c == 0;
    case EXPR_T_GT:
        return c == 1;
    case EXPR_T_GE:
        return c == 1 || c == 0;
    case EXPR_T_EQ:
        return c == 0;
    default:
        return c != 0;
    }
}

static int expr_prec(expr_tok_t tok)
{
    switch (tok)
    {
    case EXPR_T_BOR:
        return 1;
    case EXPR_T_XOR:
        return 2;
    case EXPR_T_BAND:
        return 3;
    case EXPR_T_SHL:
    case EXPR_T_SHR:
        return 4;
    case EXPR_T_ADD:
    case EXPR_T_SUB:
        return 5;
    case EXPR_T_MUL:
    case EXPR_T_DIV:
    case EXPR_T_FLOORDIV:
    case EXPR_T_MOD:
        return 6;
    default:
        return 0;
    }
}

static expr_status_t expr_or(expr_t *e, expr_value_t *v);
static expr_status_t expr_unary(expr_t *e, expr_value_t *v);

static expr_status_t expr_literal(expr_t *e, expr_value_t *v)
{
    if (!e->is_float)
    {
        if (e->big || e->whole > INT64_MAX)
            return EXPR_OVERFLOW;
        v->i = (int64_t)e->whole;
        return EXPR_OK;
    }
    char *buf = malloc(e->len + 1);
    if (!buf)
        return EXPR_BAD;
    size_t n = 0;
    for (size_t i = 0; i < e->len; i++)
        if (e->text[i] != '_')
            buf[n++] = e->text[i];
    buf[n] = 0;
    v->is_float = true;
    v->f = strtod(buf, NULL);
    free(buf);
    return EXPR_OK;
}

static expr_status_t expr_atom(expr_t *e, expr_value_t *v)
{
    expr_status_t st = EXPR_OK;
    v->is_float = false;
    v->i = 0;
    switch (e->tok)
    {
    case EXPR_T_NUM:
        if (!e->skip)
            st = expr_literal(e, v);
        break;
    case EXPR_T_NAME:
        if (!e->skip && (st = e->lookup(e->text, e->len, v)) != EXPR_OK)
        {
            *e->name = e->text;
            *e->name_len = e->len;
        }
        break;
    case EXPR_T_OPEN:
        expr_next(e);
        st = expr_or(e, v);
        if (st == EXPR_OK && e->tok != EXPR_T_CLOSE)
            st = EXPR_BAD;
        break;
    default:
        return EXPR_BAD;
    }
    if (st == EXPR_OK)
        expr_next(e);
    return st;
}

/* ** takes a unary minus on its right, as in 2 ** -1, and works right to
 * left. */
static expr_status_t expr_power(expr_t *e, expr_value_t *v)
{
    expr_status_t st = expr_atom(e, v);
    if (st != EXPR_OK || e->tok != EXPR_T_POW)
        return st;
    expr_next(e);
    expr_value_t r;
    st = expr_unary(e, &r);
    if (st != EXPR_OK || e->skip)
        return st;
    return expr_binary(EXPR_T_POW, v, &r);
}

static expr_status_t expr_unary(expr_t *e, expr_value_t *v)
{
    expr_tok_t op = e->tok;
    if (op != EXPR_T_SUB && op != EXPR_T_ADD && op != EXPR_T_INV)
        return expr_power(e, v);
    expr_next(e);
    expr_status_t st = expr_unary(e, v);
    if (st != EXPR_OK || e->skip || op == EXPR_T_ADD)
        return st;
    if (op == EXPR_T_INV)
    {
        if (v->is_float)
            return EXPR_NOT_WHOLE;
        v->i = ~v->i;
    }
    else if (v->is_float)
        v->f = -v->f;
    else if (v->i == INT64_MIN)
        return EXPR_OVERFLOW;
    else
        v->i = -v->i;
    return EXPR_OK;
}

/* Parses the binary operators | ^ & << >> + - * / // %, and stops at an
 * operator whose precedence is below min. */
static expr_status_t expr_arith(expr_t *e, expr_value_t *v, int min)
{
    expr_status_t st = expr_unary(e, v);
    int prec;
    while (st == EXPR_OK && (prec = expr_prec(e->tok)) >= min)
    {
        expr_tok_t op = e->tok;
        expr_next(e);
        expr_value_t r;
        st = expr_arith(e, &r, prec + 1);
        if (st == EXPR_OK && !e->skip)
            st = expr_binary(op, v, &r);
    }
    return st;
}

/* a < b < c is a < b and b < c, with b worked out once and c skipped when
 * a < b is false. */
static expr_status_t expr_compare(expr_t *e, expr_value_t *v)
{
    expr_status_t st = expr_arith(e, v, 1);
    if (st != EXPR_OK || e->tok < EXPR_T_LT || e->tok > EXPR_T_NE)
        return st;
    bool skip = e->skip, holds = true;
    expr_value_t x = *v;
    while (st == EXPR_OK && e->tok >= EXPR_T_LT && e->tok <= EXPR_T_NE)
    {
        expr_tok_t op = e->tok;
        expr_next(e);
        expr_value_t y;
        st = expr_arith(e, &y, 1);
        if (st == EXPR_OK && !e->skip)
        {
            holds = expr_holds(op, &x, &y);
            e->skip = !holds;
            x = y;
        }
    }
    e->skip = skip;
    v->is_float = false;
    v->i = holds;
    return st;
}

static expr_status_t expr_not(expr_t *e, expr_value_t *v)
{
    if (e->tok != EXPR_T_NOT)
        return expr_compare(e, v);
    expr_next(e);
    expr_status_t st = expr_not(e, v);
    if (st == EXPR_OK && !e->skip)
    {
        v->i = !expr_true(v);
        v->is_float = false;
    }
    return st;
}

/* a and b is a when a is zero, and then b is skipped. Otherwise it is b. */
static expr_status_t expr_and(expr_t *e, expr_value_t *v)
{
    expr_status_t st = expr_not(e, v);
    while (st == EXPR_OK && e->tok == EXPR_T_AND)
    {
        bool skip = e->skip;
        e->skip = skip || !expr_true(v);
        expr_next(e);
        expr_value_t r;
        st = expr_not(e, &r);
        if (!e->skip)
            *v = r;
        e->skip = skip;
    }
    return st;
}

/* a or b is a when a is not zero, and then b is skipped. Otherwise it is b. */
static expr_status_t expr_or(expr_t *e, expr_value_t *v)
{
    expr_status_t st = expr_and(e, v);
    while (st == EXPR_OK && e->tok == EXPR_T_OR)
    {
        bool skip = e->skip;
        e->skip = skip || expr_true(v);
        expr_next(e);
        expr_value_t r;
        st = expr_and(e, &r);
        if (!e->skip)
            *v = r;
        e->skip = skip;
    }
    return st;
}

expr_status_t expr_eval(const char *text, size_t len, expr_lookup_t lookup,
                        expr_value_t *out, const char **name, size_t *name_len)
{
    expr_t e = {
        .end = text + len,
        .lookup = lookup,
        .name = name,
        .name_len = name_len,
    };
    /* The first pass only checks syntax, so a syntax error is reported before
     * any other error. */
    for (int pass = 0; pass < 2; pass++)
    {
        e.s = text;
        e.skip = pass == 0;
        expr_next(&e);
        expr_status_t st = expr_or(&e, out);
        if (st == EXPR_OK && e.tok != EXPR_T_END)
            st = EXPR_BAD;
        if (st != EXPR_OK)
            return st;
    }
    return EXPR_OK;
}

bool expr_int(const char *text, size_t len, int64_t *out)
{
    const char *end = text + len;
    bool neg = len && *text == '-';
    text += neg;
    uint64_t whole;
    bool big, is_float;
    const char *s = text < end ? expr_number(text, end, &whole, &big, &is_float)
                               : NULL;
    if (s != end || is_float || big || whole > (uint64_t)INT64_MAX + neg)
        return false;
    *out = neg && whole ? -(int64_t)(whole - 1) - 1 : (int64_t)whole;
    return true;
}
