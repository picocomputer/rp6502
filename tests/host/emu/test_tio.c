/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* tio over sources this file stands in for, each queue holding the number of
 * bytes a test gives it, and one byte that can be staged ahead of a source as
 * the 6502's receive register holds it. The machine clock is stood in for
 * too. */

#include "core/com/tio.h"
#include "core/sys/timer.h"
#include "utest.h"
#include <string.h>

static struct
{
    char q[256];
    size_t len;
    size_t size;
} fake[COM_SOURCE_COUNT];

static com_source_t fake_staged_src = COM_SOURCE_ANY;
static char fake_staged;

/* Bytes past the queue's size are dropped, as a full ring drops them. */
static void push(com_source_t src, const char *s)
{
    for (; *s && fake[src].len < fake[src].size; s++)
        fake[src].q[fake[src].len++] = *s;
}

static void stage(com_source_t src, char c)
{
    fake_staged_src = src;
    fake_staged = c;
}

static uint64_t fake_now;

static void sources(size_t keyboard, size_t uart)
{
    memset(fake, 0, sizeof fake);
    fake[COM_SOURCE_KEYBOARD].size = keyboard;
    fake[COM_SOURCE_UART].size = uart;
    fake[COM_SOURCE_TEL].size = 32;
    fake_staged_src = COM_SOURCE_ANY;
    tio_init();
}

static void later_ms(uint64_t ms)
{
    fake_now += ms * 1000;
}

timer_mach_t timer_mach_in_ms(uint64_t ms)
{
    return fake_now + ms * 1000;
}

bool timer_mach_passed(timer_mach_t d)
{
    return (int64_t)(fake_now - d) >= 0;
}

size_t com_read(com_source_t src, char *buf, size_t n)
{
    size_t got = 0;
    if (n && fake_staged_src == src)
    {
        buf[got++] = fake_staged;
        fake_staged_src = COM_SOURCE_ANY;
    }
    size_t take = fake[src].len < n - got ? fake[src].len : n - got;
    memcpy(&buf[got], fake[src].q, take);
    memmove(fake[src].q, &fake[src].q[take], fake[src].len - take);
    fake[src].len -= take;
    return got + take;
}

int com_rx_peek(com_source_t src)
{
    return fake_staged_src == src ? (unsigned char)fake_staged : -1;
}

int com_peek(com_source_t src, size_t offset)
{
    if (fake_staged_src == src)
    {
        if (!offset)
            return (unsigned char)fake_staged;
        offset--;
    }
    return offset < fake[src].len ? (unsigned char)fake[src].q[offset] : -1;
}

size_t com_queue_size(com_source_t src)
{
    return fake[src].size;
}

static char got[TIO_ITEM_MAX + 1];
static com_source_t got_src;

/* The next item as a string, empty when there is none. */
static const char *next(void)
{
    size_t n = tio_read(&got_src, got);
    got[n] = 0;
    return got;
}

static char seq[300];

/* ESC '[', count digits, then the tail. */
static const char *csi_of(size_t count, const char *tail)
{
    strcpy(seq, "\33[");
    memset(seq + 2, '1', count);
    strcpy(seq + 2 + count, tail);
    return seq;
}

UTEST(tio, each_item_ends_by_the_framing_rules)
{
    sources(256, 256);
    push(COM_SOURCE_UART, "a\33[1;5C\33OA\33Nx\33b\33\177\33\r\33[?25h\33[ q");
    ASSERT_STREQ(next(), "a");
    ASSERT_EQ(got_src, COM_SOURCE_UART);
    ASSERT_STREQ(next(), "\33[1;5C");
    ASSERT_STREQ(next(), "\33OA");
    ASSERT_STREQ(next(), "\33Nx");
    ASSERT_STREQ(next(), "\33b");
    ASSERT_STREQ(next(), "\33\177");
    ASSERT_STREQ(next(), "\33\r");
    ASSERT_STREQ(next(), "\33[?25h");
    ASSERT_STREQ(next(), "\33[ q");
    ASSERT_STREQ(next(), "");
}

UTEST(tio, can_and_sub_drop_the_sequence_they_land_in_and_are_keys_outside_one)
{
    sources(256, 256);
    push(COM_SOURCE_UART, "\33[12\30a\33O\32b\30\32");
    ASSERT_STREQ(next(), "a");
    ASSERT_STREQ(next(), "b");
    ASSERT_STREQ(next(), "\30");
    ASSERT_STREQ(next(), "\32");
    ASSERT_STREQ(next(), "");
}

UTEST(tio, an_esc_inside_a_sequence_starts_it_over)
{
    sources(256, 256);
    push(COM_SOURCE_UART, "\33\33[A\33[1\33[B\33O\33[C");
    ASSERT_STREQ(next(), "\33[A");
    ASSERT_STREQ(next(), "\33[B");
    ASSERT_STREQ(next(), "\33[C");
    ASSERT_STREQ(next(), "");
}

UTEST(tio, a_control_inside_a_sequence_ends_it_and_stands_alone)
{
    sources(256, 256);
    push(COM_SOURCE_UART, "\33[\r\33[1\177\33[1;\n\33O\b\33Nx");
    ASSERT_STREQ(next(), "\r");
    ASSERT_STREQ(next(), "\177");
    ASSERT_STREQ(next(), "\n");
    ASSERT_STREQ(next(), "\b");
    ASSERT_STREQ(next(), "\33Nx");
    ASSERT_STREQ(next(), "");
}

/* A UTF-8 character typed into an unfinished sequence comes through whole. */
UTEST(tio, a_byte_of_0x80_and_up_ends_a_sequence_and_stands_alone)
{
    sources(256, 256);
    push(COM_SOURCE_UART, "\33[1\303\251\33\303\251");
    ASSERT_STREQ(next(), "\303");
    ASSERT_STREQ(next(), "\251");
    ASSERT_STREQ(next(), "\303");
    ASSERT_STREQ(next(), "\251");
    ASSERT_STREQ(next(), "");
}

UTEST(tio, intermediates_keep_a_csi_open_to_its_final_byte)
{
    sources(256, 256);
    push(COM_SOURCE_UART, "\33[?2004;1$yx\33[4;2$yx\33[2$x\33[11$x");
    ASSERT_STREQ(next(), "\33[?2004;1$y");
    ASSERT_STREQ(next(), "x");
    ASSERT_STREQ(next(), "\33[4;2$y");
    ASSERT_STREQ(next(), "x");
    ASSERT_STREQ(next(), "\33[2$");
    ASSERT_STREQ(next(), "x");
    ASSERT_STREQ(next(), "\33[11$");
    ASSERT_STREQ(next(), "x");
}

UTEST(tio, linux_console_function_keys_are_one_item)
{
    sources(256, 256);
    push(COM_SOURCE_UART, "\33[[A\33[[Ex");
    ASSERT_STREQ(next(), "\33[[A");
    ASSERT_STREQ(next(), "\33[[E");
    ASSERT_STREQ(next(), "x");
}

UTEST(tio, ss3_keys_take_parameters)
{
    sources(256, 256);
    push(COM_SOURCE_UART, "\33O5P\33O1;5Ax\33OMy");
    ASSERT_STREQ(next(), "\33O5P");
    ASSERT_STREQ(next(), "\33O1;5A");
    ASSERT_STREQ(next(), "x");
    ASSERT_STREQ(next(), "\33OM");
    ASSERT_STREQ(next(), "y");
}

UTEST(tio, an_x10_mouse_report_takes_its_three_bytes)
{
    sources(256, 256);
    push(COM_SOURCE_UART, "\33[M !\377x\33[2Mx");
    ASSERT_STREQ(next(), "\33[M !\377");
    ASSERT_STREQ(next(), "x");
    ASSERT_STREQ(next(), "\33[2M");
    ASSERT_STREQ(next(), "x");
}

static bool parse(const char *item, tio_csi_t *csi)
{
    return tio_csi_parse(item, strlen(item), csi);
}

UTEST(tio, a_csi_parses_only_in_its_own_shape)
{
    tio_csi_t csi;
    ASSERT_TRUE(parse("\33[?25h", &csi));
    ASSERT_EQ(csi.priv, '?');
    ASSERT_EQ(csi.param[0], 25);
    ASSERT_FALSE(parse("\33[1?5h", &csi));
    ASSERT_FALSE(parse("\33[1$5y", &csi));
    ASSERT_TRUE(parse("\33[1:3;5R", &csi));
    ASSERT_EQ(csi.count, 2);
    ASSERT_EQ(csi.param[0], 1);
    ASSERT_EQ(csi.param[1], 5);
    ASSERT_TRUE(parse("\33[?2004;1$y", &csi));
    ASSERT_EQ(csi.inter, '$');
    ASSERT_EQ(csi.final, 'y');
    ASSERT_TRUE(parse("\33[7$", &csi));
    ASSERT_EQ(csi.inter, 0);
    ASSERT_EQ(csi.final, '$');
    ASSERT_EQ(csi.param[0], 7);
    ASSERT_TRUE(parse("\33[M !!", &csi));
    ASSERT_EQ(csi.final, 'M');
    ASSERT_TRUE(parse("\33[[A", &csi));
    ASSERT_EQ(csi.final, '[');
    ASSERT_FALSE(parse("\33[1M!!!", &csi));
    ASSERT_FALSE(parse("\33[AB", &csi));
}

/* ----- A sequence that does not finish ----- */

UTEST(tio, a_lone_esc_is_the_escape_key_after_a_quiet_time)
{
    sources(256, 256);
    push(COM_SOURCE_UART, "\33");
    ASSERT_STREQ(next(), "");
    later_ms(TIO_ESC_MS - 1);
    ASSERT_STREQ(next(), "");
    ASSERT_TRUE(tio_idle());
    later_ms(1);
    ASSERT_FALSE(tio_idle());
    ASSERT_STREQ(next(), "\33");
    ASSERT_STREQ(next(), "");
}

/* Alt+[ is ESC '[', which would otherwise take the next key as its final. */
UTEST(tio, an_unfinished_sequence_is_dropped_after_a_quiet_time)
{
    sources(256, 256);
    push(COM_SOURCE_UART, "\33[");
    ASSERT_STREQ(next(), "");
    later_ms(TIO_ESC_MS);
    ASSERT_STREQ(next(), "");
    push(COM_SOURCE_UART, "A");
    ASSERT_STREQ(next(), "A");
}

UTEST(tio, a_sequence_still_growing_is_not_quiet)
{
    sources(256, 256);
    push(COM_SOURCE_UART, "\33");
    ASSERT_STREQ(next(), "");
    later_ms(TIO_ESC_MS - 1);
    push(COM_SOURCE_UART, "[1");
    ASSERT_STREQ(next(), "");
    later_ms(TIO_ESC_MS - 1);
    push(COM_SOURCE_UART, "A");
    ASSERT_STREQ(next(), "\33[1A");
}

UTEST(tio, the_rest_of_an_overlong_sequence_is_given_up_after_a_quiet_time)
{
    sources(256, 256);
    push(COM_SOURCE_UART, csi_of(40, ""));
    ASSERT_STREQ(next(), "");
    ASSERT_STREQ(next(), "");
    later_ms(TIO_ESC_MS);
    ASSERT_STREQ(next(), "");
    push(COM_SOURCE_UART, "Cx");
    ASSERT_STREQ(next(), "C");
    ASSERT_STREQ(next(), "x");
}

UTEST(tio, a_sequence_still_arriving_stays_in_its_source)
{
    sources(256, 256);
    push(COM_SOURCE_UART, "\33[1;");
    ASSERT_STREQ(next(), "");
    ASSERT_EQ(com_peek(COM_SOURCE_UART, 3), ';');
    push(COM_SOURCE_UART, "5C");
    ASSERT_STREQ(next(), "\33[1;5C");
}

UTEST(tio, the_keyboard_goes_first_and_a_waiting_esc_blocks_no_other_source)
{
    sources(256, 256);
    push(COM_SOURCE_UART, "u");
    push(COM_SOURCE_KEYBOARD, "k\33");
    ASSERT_STREQ(next(), "k");
    ASSERT_EQ(got_src, COM_SOURCE_KEYBOARD);
    ASSERT_STREQ(next(), "u");
    ASSERT_EQ(got_src, COM_SOURCE_UART);
    ASSERT_STREQ(next(), "");
}

UTEST(tio, an_item_of_32_bytes_is_whole)
{
    sources(256, 256);
    push(COM_SOURCE_UART, csi_of(29, "C"));
    ASSERT_EQ(strlen(next()), (size_t)32);
}

UTEST(tio, a_sequence_open_at_32_bytes_is_dropped_to_its_end)
{
    sources(256, 256);
    push(COM_SOURCE_UART, csi_of(30, ""));
    ASSERT_STREQ(next(), "");
    ASSERT_EQ(com_peek(COM_SOURCE_UART, 0), -1);
    push(COM_SOURCE_UART, csi_of(0, ";5Cx") + 2);
    ASSERT_STREQ(next(), "x");
    push(COM_SOURCE_UART, csi_of(100, "Cy"));
    ASSERT_STREQ(next(), "y");
}

UTEST(tio, a_16_byte_queue_ends_a_sequence_at_16)
{
    sources(16, 256);
    push(COM_SOURCE_KEYBOARD, csi_of(13, "C"));
    ASSERT_EQ(strlen(next()), (size_t)16);
    push(COM_SOURCE_KEYBOARD, csi_of(20, ""));
    ASSERT_EQ(fake[COM_SOURCE_KEYBOARD].len, (size_t)16);
    ASSERT_STREQ(next(), "");
    ASSERT_EQ(fake[COM_SOURCE_KEYBOARD].len, (size_t)0);
    push(COM_SOURCE_KEYBOARD, "12Ck");
    ASSERT_STREQ(next(), "k");
    ASSERT_EQ(got_src, COM_SOURCE_KEYBOARD);
}

UTEST(tio, a_staged_byte_counts_toward_a_16_byte_queue)
{
    sources(16, 256);
    stage(COM_SOURCE_KEYBOARD, '\33');
    push(COM_SOURCE_KEYBOARD, csi_of(14, "C") + 1);
    ASSERT_EQ(strlen(next()), (size_t)17);
    stage(COM_SOURCE_KEYBOARD, '\33');
    push(COM_SOURCE_KEYBOARD, csi_of(20, "") + 1);
    ASSERT_STREQ(next(), "");
    ASSERT_EQ(com_peek(COM_SOURCE_KEYBOARD, 0), -1);
    push(COM_SOURCE_KEYBOARD, "1Ck");
    ASSERT_STREQ(next(), "k");
}

UTEST(tio, a_staged_byte_from_another_source_does_not_count)
{
    sources(16, 256);
    stage(COM_SOURCE_UART, 'u');
    push(COM_SOURCE_KEYBOARD, csi_of(20, ""));
    ASSERT_STREQ(next(), "u");
    ASSERT_STREQ(next(), "");
    ASSERT_EQ(fake[COM_SOURCE_KEYBOARD].len, (size_t)0);
}

UTEST(tio, idle_is_no_whole_item_waiting)
{
    sources(256, 256);
    ASSERT_TRUE(tio_idle());
    push(COM_SOURCE_UART, "\33");
    ASSERT_TRUE(tio_idle());
    push(COM_SOURCE_UART, "[");
    ASSERT_TRUE(tio_idle());
    push(COM_SOURCE_UART, "A");
    ASSERT_FALSE(tio_idle());
    ASSERT_STREQ(next(), "\33[A");
    push(COM_SOURCE_KEYBOARD, "\33[1\30");
    ASSERT_TRUE(tio_idle());
    push(COM_SOURCE_KEYBOARD, "x");
    ASSERT_FALSE(tio_idle());
}

UTEST(tio, an_overlong_sequence_is_no_item_for_idle)
{
    sources(256, 256);
    push(COM_SOURCE_UART, csi_of(40, ""));
    ASSERT_TRUE(tio_idle());
    push(COM_SOURCE_UART, "C");
    ASSERT_TRUE(tio_idle());
    push(COM_SOURCE_UART, "z");
    ASSERT_FALSE(tio_idle());
    ASSERT_STREQ(next(), "z");
    push(COM_SOURCE_UART, csi_of(40, ""));
    ASSERT_STREQ(next(), "");
    push(COM_SOURCE_UART, "1");
    ASSERT_TRUE(tio_idle());
    push(COM_SOURCE_UART, "Cz");
    ASSERT_FALSE(tio_idle());
}

UTEST(tio, a_read_end_drops_the_rest_of_an_overlong_sequence_and_forgets_it)
{
    sources(256, 256);
    push(COM_SOURCE_UART, csi_of(40, ""));
    ASSERT_STREQ(next(), "");
    push(COM_SOURCE_UART, "1;1");
    tio_read_end();
    ASSERT_EQ(com_peek(COM_SOURCE_UART, 0), -1);
    push(COM_SOURCE_UART, "2C");
    ASSERT_STREQ(next(), "2");
    ASSERT_STREQ(next(), "C");
}

UTEST(tio, a_read_end_leaves_a_sequence_still_arriving)
{
    sources(256, 256);
    push(COM_SOURCE_UART, "\33[6;");
    tio_read_end();
    push(COM_SOURCE_UART, "3C");
    ASSERT_STREQ(next(), "\33[6;3C");
}

/* Shift-F3 is CSI 1;2R on xterm and PuTTY, and nothing is asked, so an
 * answer-shaped sequence is an item from any source. */
UTEST(tio, a_cpr_or_da2_shaped_sequence_is_an_item)
{
    sources(256, 256);
    push(COM_SOURCE_UART, "\33[5;3R\33[>1;10;0c");
    push(COM_SOURCE_KEYBOARD, "\33[1;2R");
    ASSERT_STREQ(next(), "\33[1;2R");
    ASSERT_EQ(got_src, COM_SOURCE_KEYBOARD);
    ASSERT_STREQ(next(), "\33[5;3R");
    ASSERT_EQ(got_src, COM_SOURCE_UART);
    ASSERT_STREQ(next(), "\33[>1;10;0c");
    ASSERT_STREQ(next(), "");
}

/* ----- A telnet session that goes away ----- */

/* The session on telnet is torn down with a sequence half taken, and the
 * next session types. */
UTEST(tio, a_telnet_session_replaced_during_a_read_loses_no_key)
{
    sources(256, 256);
    push(COM_SOURCE_TEL, csi_of(40, ""));
    ASSERT_STREQ(next(), "");
    fake[COM_SOURCE_TEL].len = 0;
    tio_forget(COM_SOURCE_TEL);
    push(COM_SOURCE_TEL, "k");
    ASSERT_STREQ(next(), "k");
    ASSERT_EQ(got_src, COM_SOURCE_TEL);
}

UTEST_MAIN();
