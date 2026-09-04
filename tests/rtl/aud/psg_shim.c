/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The seams psg.c stands on, provided flat so the lockstep test runs
 * the vendored DSP against the verilated engine with no mixer in
 * between: XRAM and its write-notify queue with the RW engine's push
 * (core/ria/ria.c's), and the registration a mixer would answer. The bell
 * is the real bel.c, linked whole. The rate is the build's: this test is
 * compiled with AUD_NATIVE_RATE at the model's 48000.
 */

#include "psg_shim.h"

#include "core/aud/mix.h"
#include "core/aud/psg.h"
#include "core/aud/sine.h"
#include "core/ria/regs.h"
#include "core/sys/xram.h"

#include "aud_sine_tables.h"

static uint8_t xram_backing[0x10000];
volatile uint8_t *const xram = xram_backing;
volatile uint8_t xram_queue_page;
volatile uint8_t xram_queue_head;
volatile uint8_t xram_queue_tail;
volatile uint8_t xram_queue[256][2];

int16_t sine_table[256];

/* psg_xreg registers itself and parks itself through these. There is no
 * mixer here to tell; the bench calls psg_sample directly. */
void aud_setup(void (*sample)(int16_t *left, int16_t *right)) { (void)sample; }
void aud_stop(void) {}

void shim_init(void)
{
    for (int i = 0; i < 256; i++)
        sine_table[i] = AUD_SINE_TABLE[i];
}

void shim_sample(int16_t *l, int16_t *r)
{
    psg_sample(l, r);
}

void shim_xram_write(uint16_t addr, uint8_t val)
{
    xram_backing[addr] = val;
    if (xram_queue_page == (uint8_t)(addr >> 8))
    {
        uint8_t next = (uint8_t)(xram_queue_head + 1);
        if (next != xram_queue_tail)
        {
            xram_queue[next][0] = (uint8_t)addr;
            xram_queue[next][1] = val;
            xram_queue_head = next;
        }
    }
}

uint8_t shim_xram_read(uint16_t addr)
{
    return xram_backing[addr];
}
