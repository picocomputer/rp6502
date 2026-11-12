/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The pan of a PSG voice, bits 7-1 of pan_gate, is the same whether bit 0, the
 * gate, is set or not, so a held note is panned where the program put it, and
 * left and right pans mirror each other exactly. */

#include "core/aud/psg.h"
#include "utest.h"
#include <stdlib.h>

/* One voice holding a square at full level, so each sample is the same rail
 * value, up or down, and only the pan moves it between the sides. */
static void hold(void)
{
    psg_map(0);
    psg_put(0, (uint8_t)1320);
    psg_put(1, (uint8_t)(1320 >> 8));
    psg_put(2, 0);    /* duty */
    psg_put(3, 0x00); /* attack to full at the fastest rate */
    psg_put(4, 0x00); /* sustain at full */
    psg_put(5, 0x10); /* square, fastest release */
    psg_put(6, 0x01); /* centred, gate */
    for (int i = 0; i < 2000; i++)
    {
        int16_t l, r;
        psg_sample(&l, &r);
    }
}

static void panned(uint8_t pan_gate, int16_t *l, int16_t *r)
{
    psg_put(6, pan_gate);
    psg_sample(l, r);
    psg_sample(l, r);
}

UTEST(psg, a_held_note_pans_where_it_is_put)
{
    hold();
    for (int p = 1; p <= 63; p++)
    {
        int16_t ll, lr, rl, rr;
        panned((uint8_t)(-p * 2) | 1, &ll, &lr);
        panned((uint8_t)(p * 2) | 1, &rl, &rr);
        ASSERT_EQ(abs(ll), abs(rr));
        ASSERT_EQ(abs(lr), abs(rl));
    }
    int16_t l, r;
    panned((uint8_t)(-63 * 2) | 1, &l, &r);
    ASSERT_NE(l, 0);
    ASSERT_EQ(r, 0);
    panned((uint8_t)(-1 * 2) | 1, &l, &r);
    ASSERT_NE(l, r);
    psg_map(0xFFFF);
}

/* -64, one past the documented range, drops the voice, gate or no gate. */
UTEST(psg, a_pan_of_minus_64_is_silent)
{
    hold();
    int16_t l, r;
    panned(0x81, &l, &r);
    ASSERT_EQ(l, 0);
    ASSERT_EQ(r, 0);
    psg_map(0xFFFF);
}

UTEST_MAIN();
