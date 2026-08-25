/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The console bell. The fabric holds the voice, a ninth channel of
 * aud_psg; this side decides what it plays and when.
 *
 * The split is where the clocks are. A voice steps 48000 times a second,
 * which this processor cannot do — no interrupts, and it can sit inside
 * a file operation for milliseconds. The events it does own are 20 to
 * 800 ms apart, which a polled task handles.
 */

#include "bel.h"
#include "mmio.h"
#include "core/aud/bel.h"
#include "host.h"

#define BEL_QUEUE_SIZE 8

static ria_bel_t bel_queue[BEL_QUEUE_SIZE];
static uint8_t bel_head, bel_tail;
static bool bel_active;
static host_deadline_t bel_restrike_at, bel_release_at, bel_end_at;

/* The pan is centre and the gate is its low bit; the fabric takes the
 * edge off the register. */
static void bel_voice(const ria_bel_t *snd, bool gate)
{
    AUD_BEL_LO = (uint32_t)snd->freq
                 | ((uint32_t)snd->duty << 16)
                 | ((uint32_t)snd->vol_attack << 24);
    AUD_BEL_HI = (uint32_t)snd->vol_decay
                 | ((uint32_t)snd->wave_release << 8)
                 | ((uint32_t)(gate ? 1u : 0u) << 16);
}

/* The voice keeps what the last session gated into it, so a host reset
 * would ring forever with nothing here counting down its release. */
void bel_init(void)
{
    AUD_BEL_LO = 0;
    AUD_BEL_HI = 0;
}

static void bel_strike(void)
{
    const ria_bel_t *snd = &bel_queue[bel_tail];
    bel_voice(snd, true);
    bel_restrike_at = snd->restrike_ms
                          ? host_deadline_ms(snd->restrike_ms) : 0;
    bel_release_at = snd->release_ms
                         ? host_deadline_ms(snd->release_ms) : 0;
    bel_end_at = snd->end_ms ? host_deadline_ms(snd->end_ms) : 0;
    bel_active = true;
}

void bel_add(const ria_bel_t *sound)
{
    uint8_t next = (bel_head + 1) % BEL_QUEUE_SIZE;
    if (next == bel_tail)
        return; // Queue full, drop
    bel_queue[bel_head] = *sound;
    bel_head = next;
    if (!bel_active)
        bel_strike();
}

void bel_task(void)
{
    if (!bel_active)
        return;

    /* A restrike takes both sounds asking for one. Where the next does
     * not, this one runs out its own life instead. */
    if (bel_restrike_at && host_deadline_passed(bel_restrike_at))
    {
        uint8_t next = (bel_tail + 1) % BEL_QUEUE_SIZE;
        if (next != bel_head && bel_queue[next].restrike_ms)
        {
            bel_tail = next;
            bel_strike();
            return;
        }
        bel_restrike_at = 0;
    }

    if (bel_release_at && host_deadline_passed(bel_release_at))
    {
        bel_voice(&bel_queue[bel_tail], false);
        bel_release_at = 0;
    }

    if (bel_end_at && host_deadline_passed(bel_end_at))
    {
        bel_tail = (bel_tail + 1) % BEL_QUEUE_SIZE;
        if (bel_tail != bel_head)
        {
            bel_strike();
        }
        else
        {
            /* Zero rather than leave it in a release with nobody
             * watching; a cleared word's release nibble is the shortest. */
            AUD_BEL_LO = 0;
            AUD_BEL_HI = 0;
            bel_restrike_at = 0;
            bel_end_at = 0;
            bel_active = false;
        }
    }
}
