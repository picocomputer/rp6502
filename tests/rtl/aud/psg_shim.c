/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The seams psg.c stands on, provided flat so the lockstep test runs
 * the vendored DSP against the verilated engine with no emulator in
 * between: XRAM and its write-notify queue with the RW engine's push
 * (core/ria/ria.c's), and the sample output capture. The bell is the
 * real bel.c, linked whole.
 */

#include "psg_shim.h"

#include "core/aud/aud.h"
#include "core/aud/bel.h"
#include "core/mem.h"

#include "aud_sine_tables.h"

static uint8_t xram_backing[0x10000];
volatile uint8_t *const xram = xram_backing;
volatile uint8_t xram_queue_page;
volatile uint8_t xram_queue_head;
volatile uint8_t xram_queue_tail;
volatile uint8_t xram_queue[256][2];

int16_t aud_sine_table[256];

static void (*handler)(void);
static int16_t out_l, out_r;

void aud_setup(void (*fn)(void), uint32_t rate)
{
    (void)rate;
    handler = fn;
}

/* The bench steps the engine itself, so the only thing this answers is what
 * bel_setup registers with — and that has to be the rate the model was
 * elaborated for. */
uint32_t aud_native_rate(void) { return PSG_SHIM_RATE; }

void aud_out(int16_t left, int16_t right)
{
    out_l = left;
    out_r = right;
}

void aud_clear_irq(void) {}

/* core/aud/aud.c's: giving up control hands the interrupt back to the
 * standing bell, which is what psg_xreg does when its pointer parks. */
void aud_stop(void)
{
    bel_setup();
}

void shim_init(void)
{
    for (int i = 0; i < 256; i++)
        aud_sine_table[i] = AUD_SINE_TABLE[i];
}

void shim_sample(int16_t *l, int16_t *r)
{
    handler();
    *l = out_l;
    *r = out_r;
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
