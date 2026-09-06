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
#include "core/ria/ria.h"
#include "core/sys/com.h"
#include "core/sys/com_term.h"
#include "core/sys/sys.h"
#include "utest.h"
#include <string.h>

static void drain(void)
{
    char buf[COM_RING_SIZE * 2];
    while (com_stdin_read(buf, sizeof buf))
        ;
}

UTEST(com, a_source_keeps_the_reader_until_it_runs_dry)
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
    /* Dry, so the wire has it now. */
    ASSERT_EQ(com_stdin_read(one, 1), (size_t)1);
    ASSERT_EQ(one[0], 'X');
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

UTEST_MAIN();
