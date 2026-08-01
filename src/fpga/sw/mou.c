/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The mouse, published as ria/hid/mou.c publishes it: five bytes at the
 * address xreg 0,0,1 names, and x and y are positions rather than
 * deltas — a sixteen-bit accumulator reported at half resolution, which
 * wraps, and which a program reads twice and subtracts. That is the
 * contract, so the accumulating happens here.
 *
 * APF sends reports, and a report that says the hand did not move is
 * the same two zeros as no report at all. So the counter it stamps each
 * report with is the only way to tell one from the other, and deltas
 * are taken exactly once per new stamp. Reading them every poll instead
 * would integrate the last movement forever and the pointer would slide
 * on its own.
 *
 * No wheel: Analogue documents buttons, X and Y for the docked mouse
 * and nothing else, so the wheel and pan bytes stay zero rather than
 * carrying a guess.
 */

#include "mmio.h"
#include "mou.h"

#include <string.h>

static struct {
    uint8_t buttons;
    uint8_t x;
    uint8_t y;
    uint8_t wheel;
    uint8_t pan;
} mou_state;

static uint16_t mou_x, mou_y;
static uint16_t mou_seen;
static bool mou_have_seen;
static uint16_t mou_xram = 0xFFFF;

bool mou_set_xram(uint16_t word)
{
    if (word != 0xFFFF && word > 0x10000 - sizeof(mou_state))
        return false;
    mou_xram = word;
    if (mou_xram != 0xFFFF)
        memcpy((void *)&XRAM_WIN[mou_xram], &mou_state, sizeof(mou_state));
    return true;
}

void mou_task(void)
{
    uint32_t key = MMIO_MOU_KEY;
    uint32_t joy = MMIO_MOU_JOY;

    if ((key >> 28) != 5) {
        mou_have_seen = false;
        return;
    }

    uint16_t count = (uint16_t)key;
    if (mou_have_seen && count == mou_seen)
        return;
    /* The first report after a mouse appears sets the mark; its deltas
     * are however far the hand moved before anyone was listening. */
    bool first = !mou_have_seen;
    mou_seen = count;
    mou_have_seen = true;

    mou_state.buttons = (uint8_t)(joy >> 16);
    if (!first) {
        mou_x += (uint16_t)joy;
        mou_y += (uint16_t)MMIO_MOU_TRIG;
    }
    mou_state.x = (uint8_t)(mou_x >> 1);
    mou_state.y = (uint8_t)(mou_y >> 1);

    if (mou_xram != 0xFFFF)
        memcpy((void *)&XRAM_WIN[mou_xram], &mou_state, sizeof(mou_state));
}

void mou_stop(void)
{
    mou_xram = 0xFFFF;
}
