/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/api/arg.h"
#include "core/api/proc.h"
#include "core/com/manifold.h"
#include "core/hid/vtkeys.h"
#include "core/ria/ria.h"
#include "core/com/com.h"
#include "core/com/tio.h"
#include "core/sys/proc.h"
#include "core/sys/ria.h"
#include "core/sys/sys.h"
#include "core/sys/timer.h"
#include "core/vga/vga_emu.h"
#include "machine.h"
#include "utest.h"
#include <string.h>

/* The machine clock counts scanlines, so a wait on it has to advance the beam
 * with vga_task. */
static void machine_us(uint64_t us)
{
    timer_mach_t d = timer_mach_in_us(us);
    while (!timer_mach_passed(d))
        vga_task();
}

static void dwell(void)
{
    machine_us(2 * COM_DWELL_US);
}

/* A byte waits in some source, a held terminal answer included. */
static bool queued(void)
{
    for (com_source_t s = COM_SOURCE_KEYBOARD; s < COM_SOURCE_COUNT; s++)
        if (com_peek(s, 0) >= 0)
            return true;
    return false;
}

static void drain(void)
{
    char buf[MANIFOLD_RING_SIZE * 2];
    timer_mach_t give_up = timer_mach_in_ms(50);
    while (queued() && !timer_mach_passed(give_up))
    {
        com_stdin_read(buf, sizeof buf);
        vga_task();
    }
}

UTEST(com, a_source_keeps_the_reader_until_it_runs_dry_and_a_dwell_past)
{
    com_rx_clear();
    manifold_keyboard_push("abc", 3);
    char one[1];
    ASSERT_EQ(com_stdin_read(one, 1), (size_t)1);
    ASSERT_EQ(one[0], 'a');
    manifold_uart_push("XY", 2);
    ASSERT_EQ(com_stdin_read(one, 1), (size_t)1);
    ASSERT_EQ(one[0], 'b');
    ASSERT_EQ(com_stdin_read(one, 1), (size_t)1);
    ASSERT_EQ(one[0], 'c');
    ASSERT_EQ(com_stdin_read(one, 1), (size_t)0);
    dwell();
    ASSERT_EQ(com_stdin_read(one, 1), (size_t)1);
    ASSERT_EQ(one[0], 'X');
    drain();
}

UTEST(com, one_read_is_one_source_and_the_keyboard_goes_first)
{
    com_rx_clear();
    manifold_keyboard_push("ab", 2);
    manifold_uart_push("XY", 2);
    char buf[8];
    ASSERT_EQ(com_stdin_read(buf, sizeof buf), (size_t)2);
    ASSERT_EQ(memcmp(buf, "ab", 2), 0);
    dwell();
    ASSERT_EQ(com_stdin_read(buf, sizeof buf), (size_t)2);
    ASSERT_EQ(memcmp(buf, "XY", 2), 0);
    drain();
}

UTEST(com, a_terminal_answer_never_jumps_ahead_of_the_wire)
{
    com_rx_clear();
    manifold_uart_push("hello", 5);
    manifold_reply("\33[1;1R", 6);
    char buf[32];
    size_t n = com_stdin_read(buf, sizeof buf);
    ASSERT_EQ(n, (size_t)11);
    ASSERT_EQ(memcmp(buf, "hello\33[1;1R", 11), 0);
    drain();
}

UTEST(com, a_held_answer_is_read_by_the_source_that_owes_it)
{
    com_rx_clear();
    manifold_reply("\33[0n", 4);
    ASSERT_EQ(com_peek(COM_SOURCE_UART, 0), 0x1b);
    com_source_t src = COM_SOURCE_UART;
    ASSERT_EQ(com_getchar(&src), 0x1b);
    ASSERT_EQ(src, COM_SOURCE_UART);
    drain();
}

UTEST(com, each_row_says_how_much_its_queue_holds)
{
    ASSERT_EQ(com_queue_size(COM_SOURCE_KEYBOARD), (size_t)MANIFOLD_RING_SIZE);
    ASSERT_EQ(com_queue_size(COM_SOURCE_UART), (size_t)MANIFOLD_RING_SIZE);
    ASSERT_EQ(com_queue_size(COM_SOURCE_TEL), (size_t)0);
}

UTEST(com, a_peek_at_an_offset_takes_nothing)
{
    com_rx_clear();
    manifold_keyboard_push("abc", 3);
    ASSERT_EQ(com_peek(COM_SOURCE_KEYBOARD, 0), 'a');
    ASSERT_EQ(com_peek(COM_SOURCE_KEYBOARD, 2), 'c');
    ASSERT_EQ(com_peek(COM_SOURCE_KEYBOARD, 3), -1);
    ASSERT_EQ(com_peek(COM_SOURCE_UART, 0), -1);
    ASSERT_EQ(com_peek(COM_SOURCE_TEL, 0), -1);
    char buf[8];
    ASSERT_EQ(com_read(COM_SOURCE_KEYBOARD, buf, sizeof buf), (size_t)3);
    ASSERT_EQ(memcmp(buf, "abc", 3), 0);
}

/* A held answer is peeked where the read delivers it, after the ring. */
UTEST(com, a_uart_peek_sees_a_held_answer_after_the_ring)
{
    com_rx_clear();
    manifold_uart_push("hi", 2);
    manifold_reply("\33[0n", 4);
    ASSERT_EQ(com_peek(COM_SOURCE_UART, 1), 'i');
    ASSERT_EQ(com_peek(COM_SOURCE_UART, 2), 0x1b);
    ASSERT_EQ(com_peek(COM_SOURCE_UART, 5), 'n');
    ASSERT_EQ(com_peek(COM_SOURCE_UART, 6), -1);
    char buf[8];
    ASSERT_EQ(com_read(COM_SOURCE_UART, buf, sizeof buf), (size_t)6);
    ASSERT_EQ(memcmp(buf, "hi\33[0n", 6), 0);
    drain();
}

UTEST(com, a_staged_byte_is_offset_zero_of_its_own_source)
{
    com_rx_clear();
    manifold_keyboard_push("kl", 2);
    ASSERT_TRUE(ria_reg_read(0xFFE0) & 0x40);
    ASSERT_EQ(com_peek(COM_SOURCE_KEYBOARD, 0), 'k');
    ASSERT_EQ(com_peek(COM_SOURCE_KEYBOARD, 1), 'l');
    ASSERT_EQ(com_peek(COM_SOURCE_KEYBOARD, 2), -1);
    ASSERT_EQ(com_peek(COM_SOURCE_UART, 0), -1);
    char buf[8];
    ASSERT_EQ(com_read(COM_SOURCE_KEYBOARD, buf, sizeof buf), (size_t)2);
    ASSERT_EQ(memcmp(buf, "kl", 2), 0);
    ASSERT_FALSE(ria_reg_read(0xFFE0) & 0x40);
}

UTEST(com, a_named_read_leaves_the_other_sources_queued)
{
    com_rx_clear();
    manifold_keyboard_push("ab", 2);
    manifold_uart_push("XYZ", 3);
    char buf[8];
    ASSERT_EQ(com_read(COM_SOURCE_UART, buf, 2), (size_t)2);
    ASSERT_EQ(memcmp(buf, "XY", 2), 0);
    ASSERT_EQ(com_read(COM_SOURCE_TEL, buf, sizeof buf), (size_t)0);
    ASSERT_EQ(com_peek(COM_SOURCE_KEYBOARD, 0), 'a');
    ASSERT_EQ(com_peek(COM_SOURCE_UART, 0), 'Z');
    drain();
}

UTEST(com, an_answer_too_big_to_hold_is_dropped_whole)
{
    com_rx_clear();
    char big[64];
    memset(big, 'x', sizeof big);
    manifold_reply(big, sizeof big);
    char buf[MANIFOLD_RING_SIZE];
    ASSERT_EQ(com_stdin_read(buf, sizeof buf), (size_t)0);
    drain();
}

/* A lone ESC is no item, but the UART's peek looks past it into a terminal
 * answer held behind it, and the two make whole items. */
UTEST(com, tio_is_idle_with_a_lone_esc_until_an_answer_is_held_behind_it)
{
    com_rx_clear();
    ASSERT_TRUE(tio_idle());
    manifold_uart_push("\33", 1);
    ASSERT_TRUE(tio_idle());
    manifold_reply("\33[0n", 4);
    ASSERT_FALSE(tio_idle());
    com_rx_clear();
    ASSERT_TRUE(tio_idle());
}

UTEST(com, a_break_during_a_paste_cancels_the_drip_too)
{
    com_rx_clear();
    char big[MANIFOLD_RING_SIZE * 4];
    memset(big, 'p', sizeof big - 1);
    big[sizeof big - 1] = 0;
    vtkeys_paste(big);
    vtkeys_task();
    ASSERT_TRUE(vtkeys_paste_busy());
    sys_break_request();
    sys_commit();
    ASSERT_FALSE(vtkeys_paste_busy());
    vtkeys_task();
    ASSERT_FALSE(queued());
}

UTEST(com, a_byte_the_window_staged_comes_back_to_its_own_source)
{
    com_rx_clear();
    manifold_keyboard_push("k", 1);
    /* When bit 6 is clear and a console byte is waiting, reading $FFE0 stages
     * that byte in $FFE2 and sets bit 6. */
    ASSERT_TRUE(ria_reg_read(0xFFE0) & 0x40);
    ASSERT_EQ(com_rx_peek(COM_SOURCE_UART), -1);
    ASSERT_EQ(com_rx_peek(COM_SOURCE_KEYBOARD), 'k');
    char buf[1];
    ASSERT_EQ(com_rx_reclaim(buf, 1, COM_SOURCE_UART), (size_t)0);
    ASSERT_EQ(com_rx_reclaim(buf, 0, COM_SOURCE_KEYBOARD), (size_t)0);
    ASSERT_EQ(com_rx_reclaim(buf, 1, COM_SOURCE_KEYBOARD), (size_t)1);
    ASSERT_EQ(buf[0], 'k');
    ASSERT_EQ(com_rx_peek(COM_SOURCE_KEYBOARD), -1);
    ASSERT_FALSE(ria_reg_read(0xFFE0) & 0x40);
}

UTEST(com, a_break_drops_what_the_window_staged)
{
    com_rx_clear();
    manifold_keyboard_push("k", 1);
    ASSERT_TRUE(ria_reg_read(0xFFE0) & 0x40);
    ria_break();
    ASSERT_FALSE(ria_reg_read(0xFFE0) & 0x40);
    ASSERT_EQ(com_rx_peek(COM_SOURCE_KEYBOARD), -1);
}

/* An arrow pressed in a program that reads only the keyboard bitmap: its ESC
 * waits in the window and the rest in the keyboard's queue. A stop with no ROM
 * launching drops all of it. */
UTEST(com, a_stop_drops_what_the_program_never_read)
{
    com_rx_clear();
    manifold_keyboard_push("\33[A", 3);
    ASSERT_TRUE(ria_reg_read(0xFFE0) & 0x40);
    proc_stop();
    ASSERT_FALSE(proc_exec_inflight());
    ASSERT_FALSE(ria_reg_read(0xFFE0) & 0x40);
    ASSERT_EQ(com_rx_peek(COM_SOURCE_KEYBOARD), -1);
}

/* A stop for an exec keeps the type-ahead, the byte in the window included. */
UTEST(com, an_exec_keeps_the_type_ahead)
{
    com_rx_clear();
    manifold_keyboard_push("ab", 2);
    ASSERT_TRUE(ria_reg_read(0xFFE0) & 0x40);
    arg_clear();
    arg_append("next.rp6502");
    arg_push_xstack();
    proc_api_exec();
    proc_stop();
    ASSERT_TRUE(proc_exec_inflight());
    ASSERT_TRUE(ria_reg_read(0xFFE0) & 0x40);
    ASSERT_EQ(com_rx_peek(COM_SOURCE_KEYBOARD), 'a');
    proc_exec_init();
    arg_clear();
    proc_stop();
    ASSERT_FALSE(proc_exec_inflight());
    ASSERT_EQ(com_rx_peek(COM_SOURCE_KEYBOARD), -1);
}

/* A paste goes with the rest of the type-ahead. */
UTEST(com, a_paste_goes_only_where_the_type_ahead_goes)
{
    com_rx_clear();
    vtkeys_paste("for the program");
    arg_clear();
    arg_append("next.rp6502");
    arg_push_xstack();
    proc_api_exec();
    proc_stop();
    ASSERT_TRUE(proc_exec_inflight());
    ASSERT_TRUE(vtkeys_paste_busy());
    proc_exec_init();
    arg_clear();
    proc_stop();
    ASSERT_FALSE(proc_exec_inflight());
    ASSERT_FALSE(vtkeys_paste_busy());
}

static const char *wire_data;
static size_t wire_len, wire_pos, wire_asked_max;

static size_t wire_rx(char *buf, size_t max)
{
    if (max > wire_asked_max)
        wire_asked_max = max;
    size_t n = wire_len - wire_pos;
    if (n > max)
        n = max;
    memcpy(buf, wire_data + wire_pos, n);
    wire_pos += n;
    return n;
}

static void wire(const char *data, size_t len, bool stream)
{
    wire_data = data;
    wire_len = len;
    wire_pos = 0;
    wire_asked_max = 0;
    manifold_set_wire(NULL, wire_rx, stream);
}

static void unwire(void)
{
    manifold_set_wire(NULL, NULL, false);
}

static size_t read_all(char *out, size_t max)
{
    size_t got = 0;
    for (int pass = 0; pass < 4000 && (wire_pos < wire_len || queued()); pass++)
    {
        manifold_task();
        got += com_stdin_read(out + got, max - got);
    }
    return got;
}

UTEST(com, a_ctrl_c_on_a_console_wire_latches_at_the_fill)
{
    com_rx_clear();
    ria_get_sigint();
    wire("ab\3", 3, false);
    manifold_task();
    ASSERT_TRUE(ria_get_sigint());
    char buf[8];
    ASSERT_EQ(com_stdin_read(buf, sizeof buf), (size_t)3);
    ASSERT_EQ(memcmp(buf, "ab\3", 3), 0);
    unwire();
}

UTEST(com, a_full_console_wire_is_held_then_read_to_drop)
{
    com_rx_clear();
    ria_get_sigint();
    static char data[MANIFOLD_RING_SIZE + 100 + 1];
    memset(data, 'x', MANIFOLD_RING_SIZE);
    memset(data + MANIFOLD_RING_SIZE, 'y', 100);
    data[sizeof data - 1] = 3;
    wire(data, sizeof data, false);
    for (int i = 0; i < 10; i++)
        manifold_task();
    ASSERT_EQ(wire_pos, (size_t)MANIFOLD_RING_SIZE);
    ASSERT_FALSE(ria_get_sigint());
    machine_us((COM_HOLD_MS + 20) * 1000);
    manifold_task();
    ASSERT_EQ(wire_pos, sizeof data);
    ASSERT_TRUE(ria_get_sigint());
    static char out[MANIFOLD_RING_SIZE * 2];
    size_t got = read_all(out, sizeof out);
    ASSERT_EQ(got, (size_t)MANIFOLD_RING_SIZE);
    for (size_t i = 0; i < got; i++)
        ASSERT_EQ(out[i], 'x');
    unwire();
}

UTEST(com, a_reader_that_keeps_up_loses_nothing_to_a_console_wire)
{
    com_rx_clear();
    ria_get_sigint();
    static char data[600];
    for (size_t i = 0; i < sizeof data; i++)
        data[i] = (char)('a' + i % 26);
    wire(data, sizeof data, false);
    static char out[sizeof data + 1];
    size_t got = read_all(out, sizeof out);
    ASSERT_EQ(got, sizeof data);
    ASSERT_EQ(memcmp(out, data, sizeof data), 0);
    ASSERT_LE(wire_asked_max, (size_t)MANIFOLD_RING_SIZE);
    ASSERT_FALSE(ria_get_sigint());
    unwire();
}

UTEST(com, a_stream_never_signals_and_never_loses_a_byte)
{
    com_rx_clear();
    ria_get_sigint();
    static char data[5 + 600];
    memcpy(data, "ab\3cd", 5);
    for (size_t i = 5; i < sizeof data; i++)
        data[i] = (char)('a' + i % 26);
    wire(data, sizeof data, true);
    for (int i = 0; i < 10; i++)
        manifold_task();
    ASSERT_LE(wire_pos, (size_t)MANIFOLD_RING_SIZE);
    ASSERT_FALSE(ria_get_sigint());
    static char out[sizeof data + 1];
    size_t got = read_all(out, sizeof out);
    ASSERT_EQ(got, sizeof data);
    ASSERT_EQ(memcmp(out, data, sizeof data), 0);
    ASSERT_FALSE(ria_get_sigint());
    unwire();
}

static char sent[64];
static size_t sent_len;

static void sent_tap(const char *buf, int len)
{
    memcpy(sent + sent_len, buf, (size_t)len);
    sent_len += (size_t)len;
}

static void tap(void)
{
    sent_len = 0;
    manifold_set_tx_tap(sent_tap);
}

static void untap(void)
{
    manifold_set_tx_tap(NULL);
}

UTEST(com, a_write_to_the_uart_reaches_it_as_it_is)
{
    tap();
    manifold_write_to(COM_TERM_UART, "\33[6n\n", 5);
    ASSERT_EQ(sent_len, (size_t)5);
    ASSERT_EQ(memcmp(sent, "\33[6n\n", 5), 0);
    untap();
}

UTEST(com, a_write_to_telnet_never_reaches_the_uart)
{
    tap();
    manifold_write_to(COM_TERM_TEL, "\33[6n", 4);
    ASSERT_EQ(sent_len, (size_t)0);
    untap();
}

UTEST(com, a_write_to_one_terminal_leaves_the_line_clean)
{
    com_line_set_clean();
    tap();
    manifold_write_to(COM_TERM_UART, "\33[?25l", 6);
    ASSERT_TRUE(com_line_clean());
    manifold_write_to(COM_TERM_ALL, "x", 1);
    ASSERT_FALSE(com_line_clean());
    ASSERT_EQ(sent_len, (size_t)7);
    ASSERT_EQ(memcmp(sent, "\33[?25lx", 7), 0);
    com_line_set_clean();
    untap();
}

UTEST(com, a_write_to_one_terminal_lands_between_the_output_around_it)
{
    tap();
    com_putchar('a');
    manifold_write_to(COM_TERM_UART, "b", 1);
    com_putchar('c');
    ASSERT_EQ(sent_len, (size_t)3);
    ASSERT_EQ(memcmp(sent, "abc", 3), 0);
    com_line_set_clean();
    untap();
}

UTEST(com, the_uart_is_the_one_terminal_without_telnet)
{
    ASSERT_EQ(manifold_term_in_charge(), (uint8_t)COM_TERM_UART);
    ASSERT_EQ(manifold_terms_connected(), (uint8_t)COM_TERM_UART);
}

UTEST_MAIN();
