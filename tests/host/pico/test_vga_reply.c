/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The VGA's reply to a message from the RIA (src/host/pico/vga/sys/ria.c):
 * owed by ria_ack or ria_nak, it goes from ria_task once the work the
 * message made is done (main_busy, which a case sets), and out of the window
 * around a VSYNC. What the VGA puts on its backchannel is kept here. */

#include "vga/main.h"
#include "vga/sys/ria.h"
#include "utest.h"
#include <hardware/pio.h>
#include <pico/time.h>

/* ---- the VGA around ria.c ---------------------------------------------- */

static uint64_t now_us = 1000000;
absolute_time_t get_absolute_time(void) { return now_us; }
void tight_loop_contents(void) { now_us++; }

pio_hw_t test_pio[2];
const pio_program_t ria_backchannel_tx_program;

/* The bytes put on the backchannel, in order. */
static uint8_t sent[16];
static unsigned sent_count;

void pio_sm_put(PIO pio, uint sm, uint32_t data)
{
    if (pio == RIA_BACKCHAN_PIO && sm == RIA_BACKCHAN_SM)
        sent[sent_count++ % sizeof sent] = (uint8_t)data;
}

uint pio_sm_get_tx_fifo_level(PIO pio, uint sm)
{
    (void)pio;
    (void)sm;
    return 0;
}

/* A job of the terminal pending, or a view to switch to. */
static bool busy;
bool main_busy(void) { return busy; }

/* ---- the reply ---------------------------------------------------------- */

/* With work to do, the reply waits for it to end, however many passes. */
UTEST(vga_reply, an_ack_waits_for_the_work)
{
    ria_init();
    busy = true;
    ria_ack();
    for (int pass = 0; pass < 12; pass++)
        ria_task();
    ASSERT_EQ(0u, sent_count);
    busy = false;
    ria_task();
    ASSERT_EQ(1u, sent_count);
    ASSERT_EQ(0x90, sent[0]);
    ria_task();
    ASSERT_EQ(1u, sent_count);
}

/* With none, it goes at the next pass. A nak is a reply like any other. */
UTEST(vga_reply, a_nak_goes_at_once_without_work)
{
    ria_init();
    ria_nak();
    ASSERT_EQ(0u, sent_count);
    ria_task();
    ASSERT_EQ(1u, sent_count);
    ASSERT_EQ(0xA0, sent[0]);
}

/* A reply ready inside the window around the next VSYNC waits for it to
 * close. */
UTEST(vga_reply, a_reply_waits_out_the_vsync_window)
{
    ria_init();
    ria_vsync();
    ASSERT_EQ(1u, sent_count);
    now_us += RIA_VSYNC_PERIOD_US - RIA_VSYNC_LOCKOUT_US;
    ria_ack();
    ria_task();
    ASSERT_EQ(1u, sent_count);
    now_us += 2 * RIA_VSYNC_LOCKOUT_US;
    ria_task();
    ASSERT_EQ(1u, sent_count);
    now_us += 1;
    ria_task();
    ASSERT_EQ(2u, sent_count);
    ASSERT_EQ(0x90, sent[1]);
}

/* The backchannel let go drops a reply still owed. */
UTEST(vga_reply, a_detach_drops_the_reply)
{
    ria_init();
    busy = true;
    ria_ack();
    ria_backchan(0);
    busy = false;
    ria_task();
    ASSERT_EQ(0u, sent_count);
}

UTEST_MAIN()
