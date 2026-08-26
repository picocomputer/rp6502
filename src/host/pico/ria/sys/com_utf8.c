/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * printf for a machine whose console speaks a code page and whose strings
 * are UTF-8: one byte in per callback, one OEM byte out per codepoint. The
 * monitor and the network stack print through these, which is why they are
 * their own translation unit rather than part of the console.
 */

#include "ria/sys/com.h"
#include "core/api/oem.h"

#include <pico/printf.h>

#include <stdarg.h>
#include <stdio.h>

// Per-call UTF-8 decode state used by the *_utf8 vfctprintf callbacks.
// One byte in per callback invocation; one OEM byte out per complete
// codepoint. Buffer fields are unused by the putchar variant.
typedef struct
{
    uint32_t accum;       // partial codepoint
    uint8_t continuation; // continuation bytes still expected
    char *dst;
    size_t dst_size;      // total dst capacity (incl. NUL)
    size_t bytes_written; // OEM bytes written or would-be-written
} utf8_state;

static void utf8_emit_buf(utf8_state *st, unsigned char oem)
{
    if (st->dst_size && st->bytes_written + 1 < st->dst_size)
        st->dst[st->bytes_written] = (char)oem;
    st->bytes_written++;
}

#define UTF8_FEED(st, b, EMIT)                              \
    do                                                      \
    {                                                       \
        unsigned char _b = (unsigned char)(b);              \
        if (_b < 0x80)                                      \
        {                                                   \
            if ((st)->continuation)                         \
            {                                               \
                EMIT(0x7F);                                 \
                (st)->continuation = 0;                     \
            }                                               \
            EMIT(_b);                                       \
        }                                                   \
        else if ((_b & 0xC0) == 0x80)                       \
        {                                                   \
            if (!(st)->continuation)                        \
            {                                               \
                EMIT(0x7F);                                 \
                break;                                      \
            }                                               \
            (st)->accum = ((st)->accum << 6) | (_b & 0x3F); \
            if (--(st)->continuation == 0)                  \
                EMIT(oem_from_codepoint((st)->accum));      \
        }                                                   \
        else                                                \
        {                                                   \
            if ((st)->continuation)                         \
                EMIT(0x7F);                                 \
            if ((_b & 0xE0) == 0xC0)                        \
            {                                               \
                (st)->accum = _b & 0x1F;                    \
                (st)->continuation = 1;                     \
            }                                               \
            else if ((_b & 0xF0) == 0xE0)                   \
            {                                               \
                (st)->accum = _b & 0x0F;                    \
                (st)->continuation = 2;                     \
            }                                               \
            else if ((_b & 0xF8) == 0xF0)                   \
            {                                               \
                (st)->accum = _b & 0x07;                    \
                (st)->continuation = 3;                     \
            }                                               \
            else                                            \
            {                                               \
                EMIT(0x7F);                                 \
                (st)->continuation = 0;                     \
            }                                               \
        }                                                   \
    } while (0)

static void cb_putchar(char c, void *arg)
{
    utf8_state *st = (utf8_state *)arg;
#define EMIT(x) putchar((x))
    UTF8_FEED(st, c, EMIT);
#undef EMIT
}

static void cb_buf(char c, void *arg)
{
    utf8_state *st = (utf8_state *)arg;
#define EMIT(x) utf8_emit_buf(st, (x))
    UTF8_FEED(st, c, EMIT);
#undef EMIT
}

int com_vprintf_utf8(const char *utf8_fmt, va_list va)
{
    utf8_state st = {0};
    int n = vfctprintf(cb_putchar, &st, utf8_fmt, va);
    if (st.continuation)
        putchar(0x7F);
    return n;
}

int com_printf_utf8(const char *utf8_fmt, ...)
{
    va_list va;
    va_start(va, utf8_fmt);
    int n = com_vprintf_utf8(utf8_fmt, va);
    va_end(va);
    return n;
}

int com_vsnprintf_utf8(char *dst, size_t dst_size,
                       const char *utf8_fmt, va_list va)
{
    utf8_state st = {0};
    st.dst = dst;
    st.dst_size = dst_size;
    (void)vfctprintf(cb_buf, &st, utf8_fmt, va);
    if (st.continuation)
        utf8_emit_buf(&st, 0x7F);
    if (dst_size)
    {
        size_t end = st.bytes_written < dst_size ? st.bytes_written : dst_size - 1;
        dst[end] = 0;
    }
    return (int)st.bytes_written;
}

int com_snprintf_utf8(char *dst, size_t dst_size, const char *utf8_fmt, ...)
{
    va_list va;
    va_start(va, utf8_fmt);
    int n = com_vsnprintf_utf8(dst, dst_size, utf8_fmt, va);
    va_end(va);
    return n;
}
