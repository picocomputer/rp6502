/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The bell's voice is psg.sv's ninth channel, since a voice steps 48000
 * times a second and the soft CPU has no interrupts. The deadlines here
 * fall 20 to 800 ms after a strike, so bel_task polls them.
 */

#include "bel.h"
#include "mmio.h"
#include "core/aud/bel.h"
#include "host/host.h"

#define BEL_QUEUE_SIZE 8

static ria_bel_t bel_queue[BEL_QUEUE_SIZE];
static uint8_t bel_head, bel_tail;
static bool bel_active;
static uint64_t bel_restrike_at, bel_release_at, bel_end_at;

static uint64_t bel_in_ms(uint32_t ms)
{
    return host_clock_us() + (uint64_t)ms * 1000;
}

static bool bel_due(uint64_t at)
{
    return at && (int64_t)(host_clock_us() - at) >= 0;
}

static void bel_put(unsigned i, uint8_t val)
{
    AUD_BEL = i << 8 | val;
}

/* The gate byte goes last, so the strike lands on the new sound. */
static void bel_voice(const ria_bel_t *snd)
{
    bel_put(0, (uint8_t)snd->freq);
    bel_put(1, (uint8_t)(snd->freq >> 8));
    bel_put(2, snd->duty);
    bel_put(3, snd->vol_attack);
    bel_put(4, snd->vol_decay);
    bel_put(5, snd->wave_release);
    bel_put(6, 1);
}

/* The gate first, so no step takes the zeroed sustain while it is up. */
static void bel_silence(void)
{
    bel_put(6, 0);
    for (unsigned i = 0; i < 6; i++)
        bel_put(i, 0);
}

void bel_init(void)
{
    bel_silence();
}

void bel_wake(uint64_t us)
{
    if (bel_restrike_at)
        bel_restrike_at += us;
    if (bel_release_at)
        bel_release_at += us;
    if (bel_end_at)
        bel_end_at += us;
}

static void bel_strike(void)
{
    const ria_bel_t *snd = &bel_queue[bel_tail];
    bel_voice(snd);
    bel_restrike_at = snd->restrike_ms
                          ? bel_in_ms(snd->restrike_ms) : 0;
    bel_release_at = snd->release_ms
                         ? bel_in_ms(snd->release_ms) : 0;
    bel_end_at = snd->end_ms ? bel_in_ms(snd->end_ms) : 0;
    bel_active = true;
}

void bel_add(const ria_bel_t *sound)
{
    uint8_t next = (bel_head + 1) % BEL_QUEUE_SIZE;
    if (next == bel_tail)
        return;
    bel_queue[bel_head] = *sound;
    bel_head = next;
    if (!bel_active)
        bel_strike();
}

void bel_task(void)
{
    /* The voice is parked until the wake ends, and bel_wake moves the
     * deadlines then. */
    if (!bel_active || (SST_CTL & SST_RESTORED))
        return;

    /* Polled until the end, as core/aud/bel.c does, so a sound queued
     * after the deadline restrikes at once. */
    if (bel_due(bel_restrike_at))
    {
        uint8_t next = (bel_tail + 1) % BEL_QUEUE_SIZE;
        if (next != bel_head && bel_queue[next].restrike_ms)
        {
            bel_tail = next;
            bel_strike();
            return;
        }
    }

    if (bel_due(bel_release_at))
    {
        bel_put(6, 0);
        bel_release_at = 0;
    }

    if (bel_due(bel_end_at))
    {
        bel_tail = (bel_tail + 1) % BEL_QUEUE_SIZE;
        if (bel_tail != bel_head)
        {
            bel_strike();
        }
        else
        {
            bel_silence();
            bel_restrike_at = 0;
            bel_end_at = 0;
            bel_active = false;
        }
    }
}
