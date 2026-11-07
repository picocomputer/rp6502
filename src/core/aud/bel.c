/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The bell's voice is the PSG's ninth, so this only sequences it: a queue
 * of sounds and the restrike, release and end deadlines of the one
 * sounding. The deadlines fall 20 to 800 ms after a strike, so bel_task
 * polls them.
 */

#include "core/aud/mix.h"
#include "core/aud/bel.h"
#include "core/sys/ring.h"
#include "host/host.h"
#include "machine.h"

HOST_IN_FLASH("bel_teletype") const bel_sound_t bel_teletype = {
    .freq = 1760,
    .duty = 215,          // gate open for 214 of 256 phase steps
    .vol_attack = 0x51,   // attack to -5vol, 8ms full-swing rate
    .vol_decay = 0x60,    // decay to -6vol, 6ms full-swing rate
    .wave_release = 0x39, // triangle, 750ms full-swing release rate
    .restrike_ms = 100,   // restrike at 10 Hz
    .release_ms = 20,
    .end_ms = 800,
};

HOST_IN_FLASH("bel_nfc_fail") const bel_sound_t bel_nfc_fail = {
    .freq = 330,
    .duty = 127,          // 50% square
    .vol_attack = 0x80,   // attack to -8vol, 2ms full-swing rate
    .vol_decay = 0x80,    // sustain at -8vol
    .wave_release = 0x15, // square, 168ms full-swing release rate
    .restrike_ms = 0,
    .release_ms = 200,
    .end_ms = 420,
};

HOST_IN_FLASH("bel_nfc_success_1") const bel_sound_t bel_nfc_success_1 = {
    .freq = 784,
    .duty = 255,          // gate open for 254 of 256 phase steps
    .vol_attack = 0x60,   // attack to -6vol, 2ms full-swing rate
    .vol_decay = 0x60,    // sustain at -6vol
    .wave_release = 0x03, // sine, 72ms full-swing release rate
    .restrike_ms = 0,
    .release_ms = 90,
    .end_ms = 170,
};

HOST_IN_FLASH("bel_nfc_success_2") const bel_sound_t bel_nfc_success_2 = {
    .freq = 1568,
    .duty = 255,          // gate open for 254 of 256 phase steps
    .vol_attack = 0x60,   // attack to -6vol, 2ms full-swing rate
    .vol_decay = 0x60,    // sustain at -6vol
    .wave_release = 0x06, // sine, 204ms full-swing release rate
    .restrike_ms = 0,
    .release_ms = 130,
    .end_ms = 350,
};

#define BEL_QUEUE_SIZE 8

/* The oldest sound is the one sounding. */
static RING(bel_sound_t, BEL_QUEUE_SIZE) bel_queue;
static uint64_t bel_restrike_at, bel_release_at, bel_end_at;

static uint64_t bel_in_ms(uint32_t ms)
{
    return host_clock_us() + (uint64_t)ms * 1000;
}

static bool bel_due(uint64_t at)
{
    return at && (int64_t)(host_clock_us() - at) >= 0;
}

/* The gate drops first and rises last, so every strike attacks from the
 * volume left, as a PSG voice's does, and lands on the new sound. */
static void bel_voice(const bel_sound_t *snd)
{
    aud_bel_put(6, 0);
    aud_bel_put(0, (uint8_t)snd->freq);
    aud_bel_put(1, (uint8_t)(snd->freq >> 8));
    aud_bel_put(2, snd->duty);
    aud_bel_put(3, snd->vol_attack);
    aud_bel_put(4, snd->vol_decay);
    aud_bel_put(5, snd->wave_release);
    aud_bel_put(6, 1);
}

/* The gate first, so no step takes the zeroed sustain while it is up. */
static void bel_silence(void)
{
    aud_bel_put(6, 0);
    for (unsigned i = 0; i < 6; i++)
        aud_bel_put(i, 0);
}

void bel_init(void)
{
    RING_RESET(&bel_queue);
    bel_restrike_at = bel_release_at = bel_end_at = 0;
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
    const bel_sound_t *snd = RING_AT(&bel_queue, 0);
    bel_voice(snd);
    bel_restrike_at = snd->restrike_ms
                          ? bel_in_ms(snd->restrike_ms) : 0;
    bel_release_at = snd->release_ms
                         ? bel_in_ms(snd->release_ms) : 0;
    bel_end_at = snd->end_ms ? bel_in_ms(snd->end_ms) : 0;
}

void bel_add(const bel_sound_t *sound)
{
    if (RING_PUT(&bel_queue, *sound) && RING_LEN(&bel_queue) == 1)
        bel_strike();
}

void bel_task(void)
{
    if (!RING_LEN(&bel_queue))
        return;

    /* Polled until the end, so a sound queued after the deadline restrikes
     * at once. */
    if (bel_due(bel_restrike_at))
    {
        if (RING_LEN(&bel_queue) > 1 && RING_AT(&bel_queue, 1)->restrike_ms)
        {
            RING_SKIP(&bel_queue, 1);
            bel_strike();
            return;
        }
    }

    if (bel_due(bel_release_at))
    {
        aud_bel_put(6, 0);
        bel_release_at = 0;
    }

    if (bel_due(bel_end_at))
    {
        RING_SKIP(&bel_queue, 1);
        if (RING_LEN(&bel_queue))
        {
            bel_strike();
        }
        else
        {
            bel_silence();
            bel_restrike_at = 0;
            bel_end_at = 0;
        }
    }
}

static void bel_put_sound(sst_cursor_t *c, const bel_sound_t *b)
{
    sst_put_u16(c, b->freq);
    sst_put_u8(c, b->duty);
    sst_put_u8(c, b->vol_attack);
    sst_put_u8(c, b->vol_decay);
    sst_put_u8(c, b->wave_release);
    sst_put_u16(c, b->restrike_ms);
    sst_put_u16(c, b->release_ms);
    sst_put_u16(c, b->end_ms);
}

static void bel_get_sound(sst_cursor_t *c, bel_sound_t *b)
{
    b->freq = sst_get_u16(c);
    b->duty = sst_get_u8(c);
    b->vol_attack = sst_get_u8(c);
    b->vol_decay = sst_get_u8(c);
    b->wave_release = sst_get_u8(c);
    b->restrike_ms = sst_get_u16(c);
    b->release_ms = sst_get_u16(c);
    b->end_ms = sst_get_u16(c);
}

void bel_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    const uint32_t len = RING_LEN(&bel_queue);
    static const bel_sound_t none;
    sst_put_u8(c, (uint8_t)len);
    for (uint32_t i = 0; i < BEL_QUEUE_SIZE; i++)
        bel_put_sound(c, i < len ? RING_AT(&bel_queue, i) : &none);
    sst_put_u64(c, bel_restrike_at);
    sst_put_u64(c, bel_release_at);
    sst_put_u64(c, bel_end_at);
}

bool bel_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    const uint8_t len = sst_get_u8(c);
    bel_sound_t queue[BEL_QUEUE_SIZE];
    for (int i = 0; i < BEL_QUEUE_SIZE; i++)
        bel_get_sound(c, &queue[i]);
    uint64_t restrike = sst_get_u64(c), release = sst_get_u64(c);
    uint64_t end = sst_get_u64(c);
    if (!sst_ok(c) || len > BEL_QUEUE_SIZE)
        return false;
    RING_RESET(&bel_queue);
    RING_IN(&bel_queue, queue, len);
    bel_restrike_at = restrike;
    bel_release_at = release;
    bel_end_at = end;
    return true;
}
