/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The gamepad, published as ria/hid/pad.c publishes it: ten bytes a
 * player, four players, at the address xreg 0,0,2 names. A program
 * written for the machine reads the same bytes here it reads there, so
 * nothing about this platform reaches it.
 *
 * The Pocket says what kind of controller it has in the top nibble of
 * the button word, and the answer decides the mapping. Docked with an
 * analog pad, everything lands where it belongs: the d-pad on the
 * d-pad, the sticks on the axes, the triggers on the triggers. Undocked
 * — or docked with a pad that has no analog — there is no stick, and a
 * program that steers by the left stick would find the machine dead in
 * its hand. So the d-pad drives the axes instead, full deflection, and
 * the d-pad byte is left empty rather than saying the same thing twice.
 *
 * That is not a special case for this platform so much as one place to
 * do what the machine's own documentation asks every program to do:
 * merge the d-pad and the left stick. Doing it in the driver means the
 * programs do not have to, and the ones that never did now work.
 */

#include "mmio.h"
#include "pad.h"

#include <string.h>

#define PAD_PLAYERS 4
#define PAD_BYTES 10

/* ria/hid/pad.c's own deadzone, and its 2:1 rule for splitting a
 * cardinal from a diagonal. The digital mirror has to agree with the
 * axes or a program reading both sees itself pulled two ways. */
#define PAD_DEADZONE 32

/* Analogue documents the axes only as "8-bit unsigned". Centre 0x80
 * with up at 0x00 is the convention every pad follows and the one that
 * makes this a single flip, but it is not written down anywhere, so it
 * is named here rather than buried in an expression. Wrong, and a
 * docked analog pad reads inverted or off-centre; the synthesised axes
 * below do not care either way. */
static int8_t pad_axis(uint8_t apf)
{
    return (int8_t)(apf ^ 0x80u);
}

static uint16_t pad_xram = 0xFFFF;

static uint8_t pad_encode_stick(int8_t x, int8_t y)
{
    int ax = x < 0 ? -(int)x : x;
    int ay = y < 0 ? -(int)y : y;
    uint8_t bits = 0;
    if (ax <= PAD_DEADZONE && ay <= PAD_DEADZONE)
        return 0;
    if (ax > PAD_DEADZONE && ay * 2 <= ax * 1 + ax)
        bits |= x < 0 ? 0x04 : 0x08;
    if (ay > PAD_DEADZONE && ax * 2 <= ay * 1 + ay)
        bits |= y < 0 ? 0x01 : 0x02;
    return bits;
}

bool pad_set_xram(uint16_t word)
{
    if (word != 0xFFFF && word > 0x10000 - PAD_BYTES * PAD_PLAYERS)
        return false;
    pad_xram = word;
    return true;
}

void pad_task(void)
{
    if (pad_xram == 0xFFFF)
        return;

    uint32_t key = MMIO_PAD_KEY;
    uint32_t joy = MMIO_PAD_JOY;
    uint16_t trig = (uint16_t)MMIO_PAD_TRIG;
    uint8_t type = (uint8_t)(key >> 28);

    uint8_t rec[PAD_BYTES * PAD_PLAYERS];
    memset(rec, 0, sizeof rec);

    if (type != 0) {
        /* Type 3 is the only one with sticks and triggers. */
        bool analog = type == 3;
        int8_t lx, ly, rx, ry;
        uint8_t lt, rt;

        if (analog) {
            lx = pad_axis((uint8_t)joy);
            ly = pad_axis((uint8_t)(joy >> 8));
            rx = pad_axis((uint8_t)(joy >> 16));
            ry = pad_axis((uint8_t)(joy >> 24));
            lt = (uint8_t)trig;
            rt = (uint8_t)(trig >> 8);
            /* The d-pad is its own input and says so. */
            rec[0] = (uint8_t)(key & 0x0F);
        } else {
            lx = (key & 0x04) ? -128 : (key & 0x08) ? 127 : 0;
            ly = (key & 0x01) ? -128 : (key & 0x02) ? 127 : 0;
            rx = ry = 0;
            lt = rt = 0;
        }
        rec[0] |= 0x80; /* connected */
        rec[1] = pad_encode_stick(lx, ly)
               | (uint8_t)(pad_encode_stick(rx, ry) << 4);
        rec[2] = (uint8_t)(((key >> 4) & 0x03)          /* A, B      */
               | (((key >> 6) & 0x03) << 3)             /* X, Y      */
               | (((key >> 8) & 0x03) << 6));           /* L1, R1    */
        rec[3] = (uint8_t)(((key >> 10) & 0x03)         /* L2, R2    */
               | (((key >> 14) & 0x03) << 2)            /* select... */
               | (((key >> 12) & 0x03) << 5));          /* L3, R3    */
        /* ria/hid/pad.c's cross-couplings, so a program that watches
         * one of a pair is never told less than one that watches the
         * other. */
        if ((rec[3] & 0x01) && !lt)
            lt = 255;
        if ((rec[3] & 0x02) && !rt)
            rt = 255;
        if (lt > PAD_DEADZONE)
            rec[3] |= 0x01;
        if (rt > PAD_DEADZONE)
            rec[3] |= 0x02;
        rec[4] = (uint8_t)lx;
        rec[5] = (uint8_t)ly;
        rec[6] = (uint8_t)rx;
        rec[7] = (uint8_t)ry;
        rec[8] = lt;
        rec[9] = rt;
    }

    /* Players two through four stay the zero record a disconnected
     * player reads as, since the Pocket only ever fills the first. */
    memcpy((void *)&XRAM_WIN[pad_xram], rec, sizeof rec);
}

void pad_stop(void)
{
    pad_xram = 0xFFFF;
}
