/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The console's input, where more than one source is live at once. The
 * emulator grew a second real source when the host's stdio became a wire,
 * and the contract has always promised a sticky picker that only the machine
 * with a real serial port actually had.
 */

#include "core/com/com.h"
#include "core/hid/vtkeys.h"
#include "core/com/tty.h"
#include "core/ria/ria.h"
#include "core/sys/com.h"
#include "core/sys/com_term.h"
#include "core/sys/ria.h"
#include "core/sys/sys.h"
#include "core/sys/timer.h"
#include "machine.h"
#include "utest.h"
#include <string.h>

/* A source keeps the reader for its dwell after it runs dry, so a read that
 * answers nothing is not the end of the input. */
static void dwell(void)
{
    timer_deadline_t d = timer_in_us(2 * COM_WIRE_DWELL_US);
    while (!timer_passed(d))
        ;
}

static void drain(void)
{
    char buf[COM_RING_SIZE * 2];
    timer_deadline_t give_up = timer_in_ms(50);
    while (!com_input_idle() && !timer_passed(give_up))
        com_stdin_read(buf, sizeof buf);
}

UTEST(com, a_source_keeps_the_reader_until_it_runs_dry_and_a_dwell_past)
{
    com_init();
    /* A paste is arriving at the keyboard while a wire delivers a file. */
    com_keyboard_push("abc", 3);
    char one[1];
    ASSERT_EQ(com_stdin_read(one, 1), (size_t)1);
    ASSERT_EQ(one[0], 'a');
    /* The wire speaks mid-paste. Taking it now would cut the paste in half,
     * which is the whole reason the picker is sticky. */
    com_uart_push("XY", 2);
    ASSERT_EQ(com_stdin_read(one, 1), (size_t)1);
    ASSERT_EQ(one[0], 'b');
    ASSERT_EQ(com_stdin_read(one, 1), (size_t)1);
    ASSERT_EQ(one[0], 'c');
    /* Dry -- but a ring the paste drip fills a chunk at a time is dry
     * between two chunks, so the keyboard keeps the reader for the dwell and
     * the wire waits. */
    ASSERT_EQ(com_stdin_read(one, 1), (size_t)0);
    dwell();
    ASSERT_EQ(com_stdin_read(one, 1), (size_t)1);
    ASSERT_EQ(one[0], 'X');
    drain();
}

UTEST(com, one_read_is_one_source_and_the_keyboard_goes_first)
{
    com_init();
    com_keyboard_push("ab", 2);
    com_uart_push("XY", 2);
    char buf[8];
    /* Both have bytes; a raw read gets one source's, never a splice of two,
     * and a keystroke does not queue behind a file for its first byte. */
    ASSERT_EQ(com_stdin_read(buf, sizeof buf), (size_t)2);
    ASSERT_EQ(memcmp(buf, "ab", 2), 0);
    dwell();
    ASSERT_EQ(com_stdin_read(buf, sizeof buf), (size_t)2);
    ASSERT_EQ(memcmp(buf, "XY", 2), 0);
    drain();
}

UTEST(com, a_terminal_answer_never_jumps_ahead_of_the_wire)
{
    com_init();
    /* Bytes are already on the wire when the terminal answers a query. */
    com_uart_push("hello", 5);
    com_in_write_reply("\33[1;1R", 6);
    char buf[32];
    size_t n = com_stdin_read(buf, sizeof buf);
    /* Everything the wire had comes first, then the answer. The answer used
     * to be drained ahead of the wire, which put it inside whatever the wire
     * was in the middle of.
     *
     * What this cannot promise is the other order: a wire that stops mid
     * sequence and stays stopped is a wire whose next bytes have not been
     * sent, and only the far end knows that. */
    ASSERT_EQ(n, (size_t)11);
    ASSERT_EQ(memcmp(buf, "hello\33[1;1R", 11), 0);
    drain();
}

UTEST(com, a_held_answer_is_read_by_the_source_that_owes_it)
{
    com_init();
    /* The line editor waits out a query by peeking the source it asked and
     * then reading only that source. */
    com_in_write_reply("\33[0n", 4);
    ASSERT_EQ(com_peekchar(COM_SOURCE_UART), 0x1b);
    com_source_t src = COM_SOURCE_UART;
    ASSERT_EQ(com_getchar(&src), 0x1b);
    ASSERT_EQ(src, COM_SOURCE_UART);
    drain();
}

UTEST(com, an_answer_too_big_to_hold_is_dropped_whole)
{
    com_init();
    char big[64];
    memset(big, 'x', sizeof big);
    com_in_write_reply(big, sizeof big);
    /* Half a sequence would be read as something else, so none of it goes. */
    char buf[COM_RING_SIZE];
    ASSERT_EQ(com_stdin_read(buf, sizeof buf), (size_t)0);
    drain();
}

UTEST(com, nothing_queued_anywhere_is_one_question)
{
    com_init();
    ASSERT_TRUE(com_input_idle());
    com_keyboard_push("k", 1);
    ASSERT_FALSE(com_input_idle());
    drain();
    ASSERT_TRUE(com_input_idle());
    /* A held answer counts: the input has not run out while one is waiting. */
    com_in_write_reply("\33[0n", 4);
    ASSERT_FALSE(com_input_idle());
    drain();
    ASSERT_TRUE(com_input_idle());
}

UTEST(com, a_break_during_a_paste_cancels_the_drip_too)
{
    com_init();
    /* More than a ring's worth: the drip parks the rest and refills the ring
     * a chunk at a time as it drains. */
    char big[COM_RING_SIZE * 4];
    memset(big, 'p', sizeof big - 1);
    big[sizeof big - 1] = 0;
    vtkeys_paste(big);
    vtkeys_task();
    ASSERT_TRUE(vtkeys_paste_busy());
    /* The console's break clears the ring. Without a break column of its own
     * the drip would refill it on the next pass, with the rest of a paste the
     * user had just interrupted. */
    sys_break_request();
    sys_commit();
    ASSERT_FALSE(vtkeys_paste_busy());
    vtkeys_task();
    ASSERT_TRUE(com_input_idle());
}

UTEST(com, a_byte_the_window_staged_comes_back_to_its_own_source)
{
    com_init();
    com_keyboard_push("k", 1);
    /* A $FFE0 poll commits it into $FFE2. */
    ASSERT_TRUE(ria_reg_read(0xFFE0) & 0x40);
    ASSERT_EQ(com_rx_peek(COM_SOURCE_UART), -1);
    ASSERT_EQ(com_rx_peek(COM_SOURCE_KEYBOARD), 'k');
    char buf[1];
    /* Another source's reader never takes it, and one with no room leaves it. */
    ASSERT_EQ(com_rx_reclaim(buf, 1, COM_SOURCE_UART), (size_t)0);
    ASSERT_EQ(com_rx_reclaim(buf, 0, COM_SOURCE_KEYBOARD), (size_t)0);
    ASSERT_EQ(com_rx_reclaim(buf, 1, COM_SOURCE_KEYBOARD), (size_t)1);
    ASSERT_EQ(buf[0], 'k');
    ASSERT_EQ(com_rx_peek(COM_SOURCE_KEYBOARD), -1);
    ASSERT_FALSE(ria_reg_read(0xFFE0) & 0x40);
}

UTEST(com, a_break_drops_what_the_window_staged)
{
    com_init();
    com_keyboard_push("k", 1);
    ASSERT_TRUE(ria_reg_read(0xFFE0) & 0x40);
    ria_break();
    ASSERT_FALSE(ria_reg_read(0xFFE0) & 0x40);
    ASSERT_EQ(com_rx_peek(COM_SOURCE_KEYBOARD), -1);
}

/* A scripted host wire: what the far end has sent, handed over as asked. */
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
    tty_set_wire(NULL, wire_rx, stream);
}

static void unwire(void)
{
    tty_set_wire(NULL, NULL, false);
}

/* Pump and read until the wire is spent and the console is empty. */
static size_t read_all(char *out, size_t max)
{
    size_t got = 0;
    for (int pass = 0; pass < 4000 && (wire_pos < wire_len || !com_input_idle()); pass++)
    {
        com_task();
        got += com_stdin_read(out + got, max - got);
    }
    return got;
}

UTEST(com, a_ctrl_c_on_a_console_wire_latches_at_the_fill)
{
    com_init();
    ria_get_sigint();
    wire("ab\3", 3, false);
    com_task();
    /* Latched before anything read it, and the byte still arrives. */
    ASSERT_TRUE(ria_get_sigint());
    char buf[8];
    ASSERT_EQ(com_stdin_read(buf, sizeof buf), (size_t)3);
    ASSERT_EQ(memcmp(buf, "ab\3", 3), 0);
    unwire();
}

UTEST(com, a_full_console_wire_is_held_then_read_to_drop)
{
    com_init();
    ria_get_sigint();
    static char data[COM_RING_SIZE - 1 + 100 + 1];
    memset(data, 'x', COM_RING_SIZE - 1);
    memset(data + COM_RING_SIZE - 1, 'y', 100);
    data[sizeof data - 1] = 3;
    wire(data, sizeof data, false);
    /* Nobody reads. The wire is asked for what fits and then left alone: what
     * the far end is holding is not lost while a reader might still come. */
    for (int i = 0; i < 10; i++)
        com_task();
    ASSERT_EQ(wire_pos, (size_t)COM_RING_SIZE - 1);
    ASSERT_FALSE(ria_get_sigint());
    /* The hold runs out. The wire is drained to its end, the Ctrl-C behind
     * the type-ahead latches, and what did not fit is gone. */
    timer_deadline_t d = timer_in_ms(COM_WIRE_HOLD_MS + 20);
    while (!timer_passed(d))
        ;
    com_task();
    ASSERT_EQ(wire_pos, sizeof data);
    ASSERT_TRUE(ria_get_sigint());
    static char out[COM_RING_SIZE * 2];
    size_t got = read_all(out, sizeof out);
    ASSERT_EQ(got, (size_t)COM_RING_SIZE - 1);
    for (size_t i = 0; i < got; i++)
        ASSERT_EQ(out[i], 'x');
    unwire();
}

UTEST(com, a_reader_that_keeps_up_loses_nothing_to_a_console_wire)
{
    com_init();
    ria_get_sigint();
    static char data[600];
    for (size_t i = 0; i < sizeof data; i++)
        data[i] = (char)('a' + i % 26);
    wire(data, sizeof data, false);
    static char out[sizeof data + 1];
    size_t got = read_all(out, sizeof out);
    ASSERT_EQ(got, sizeof data);
    ASSERT_EQ(memcmp(out, data, sizeof data), 0);
    /* Never asked for more than there was room for. */
    ASSERT_LE(wire_asked_max, (size_t)COM_RING_SIZE - 1);
    ASSERT_FALSE(ria_get_sigint());
    unwire();
}

UTEST(com, a_stream_never_signals_and_never_loses_a_byte)
{
    com_init();
    ria_get_sigint();
    static char data[5 + 600];
    memcpy(data, "ab\3cd", 5);
    for (size_t i = 5; i < sizeof data; i++)
        data[i] = (char)('a' + i % 26);
    wire(data, sizeof data, true);
    /* Nobody reads: the pipe is left holding what does not fit, for as long
     * as it takes, and nothing on it is a signal. */
    for (int i = 0; i < 10; i++)
        com_task();
    ASSERT_LE(wire_pos, (size_t)COM_RING_SIZE - 1);
    ASSERT_FALSE(ria_get_sigint());
    static char out[sizeof data + 1];
    size_t got = read_all(out, sizeof out);
    ASSERT_EQ(got, sizeof data);
    ASSERT_EQ(memcmp(out, data, sizeof data), 0);
    ASSERT_FALSE(ria_get_sigint());
    unwire();
}

UTEST_MAIN();
