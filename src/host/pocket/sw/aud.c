/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "aud.h"
#include "bel.h"
#include "mmio.h"

#include "core/aud/mix.h"
#include "host/host.h"


#include <string.h>

/* The pointer registers cannot be read back and the savestate does not
 * include them, so each pointer is also kept here in the TCM, which the
 * savestate does include. */
static uint16_t aud_psg_at = 0xFFFF;
static uint16_t aud_opl_at = 0xFFFF;

/* The open PCM: device's AUD_PCM_CTRL and AUD_PCM_STEP, kept for the same
 * reason. The control word is 0 while it is closed. */
static uint32_t aud_pcm_ctrl;
static uint32_t aud_pcm_step;

/* The PSG, the OPL, the bell voice and PCM have no reset input, so they
 * keep sounding through a machine reset until their registers are written.
 * Writing the OPL's pointer register, 0xFFFF included, silences the OPL,
 * writing the PSG's silences its first eight voices, and a close silences
 * PCM. The bell voice is the PSG's ninth voice and a pointer write does not
 * silence it, so bel_init clears its registers. */
void aud_init(void)
{
    aud_stop();
    AUD_PCM_CTRL = 0;
    bel_init();
}

/* A program's end takes its sound with it, so the close that std_stop makes
 * next has nothing to wait for. */
void aud_stop(void)
{
    AUD_PSG_XADDR = 0xFFFF;
    AUD_OPL_XADDR = 0xFFFF;
    aud_psg_at = 0xFFFF;
    aud_opl_at = 0xFFFF;
    AUD_PCM_CLEAR = 0;
}

bool aud_pcm_open(const pcm_fmt_t *fmt)
{
    if (aud_pcm_ctrl)
        return false;
    aud_pcm_step = (uint32_t)(pcm_step(fmt->rate) - 1);
    aud_pcm_ctrl = 1u | (fmt->bits == 16 ? 2u : 0u) | (fmt->channels == 2 ? 4u : 0u);
    AUD_PCM_STEP = aud_pcm_step;
    AUD_PCM_CTRL = aud_pcm_ctrl;
    return true;
}

void aud_pcm_close(void)
{
    aud_pcm_ctrl = 0;
    AUD_PCM_CTRL = 0;
}

/* The player only takes, so the room read first is never too much. buf is
 * in XRAM or the xstack, and a word load from XRAM gives one byte four
 * times, so each byte is a load of its own. */
uint32_t aud_pcm_write(const uint8_t *buf, uint32_t n)
{
    const volatile uint8_t *src = buf;
    const uint32_t room = PCM_BUF_SIZE - AUD_PCM_LEVEL;
    if (n > room)
        n = room;
    uint32_t i = 0;
    for (; i + 4 <= n; i += 4)
        AUD_PCM_DATA = (uint32_t)src[i] | (uint32_t)src[i + 1] << 8 |
                       (uint32_t)src[i + 2] << 16 | (uint32_t)src[i + 3] << 24;
    for (; i < n; i++)
        AUD_PCM_BYTE = src[i];
    return n;
}

uint16_t aud_pcm_level(void) { return (uint16_t)AUD_PCM_LEVEL; }

void aud_pcm_clear(void) { AUD_PCM_CLEAR = 0; }

uint32_t aud_opl_drops(void) { return AUD_OPL_DROPS; }

/* The engines never read XRAM and take their registers only from the
 * XRAM writes they snoop, so writing each byte of a block over itself
 * loads that block into the engine. */
static void aud_replay(uint16_t at, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++)
    {
        uint8_t v = XRAM_WIN[at + i];
        XRAM_WIN[at + i] = v;
    }
}

/* A9-AB and AE-AF, and the same in rows B and C, are aliases of channels
 * 0-2, so those channels hold whichever of their addresses was written
 * last, which the page cannot tell. The replay leaves the aliases out and
 * takes the channels from A0-C8, which is exact for a program that never
 * writes an alias. */
static void aud_opl_replay(uint16_t at)
{
    for (uint16_t i = 0; i < 256; i++)
    {
        uint8_t lo = i & 0x0F;
        if (i >= 0xA0 && i < 0xD0 && ((lo >= 9 && lo <= 11) || lo >= 14))
            continue;
        uint8_t v = XRAM_WIN[at + i];
        XRAM_WIN[at + i] = v;
    }
}

void aud_restore(void)
{
    uint16_t psg = aud_psg_at, opl = aud_opl_at;
    AUD_PSG_XADDR = 0xFFFF;
    AUD_OPL_XADDR = 0xFFFF;
    if (psg != 0xFFFF)
    {
        AUD_PSG_XADDR = psg;
        /* Writing the pointer releases the PSG's first eight voices the
         * next time the engine is in its idle state, which is within one
         * 48 kHz sample period of 20.8 us. The replay has to come after
         * that, or the notes it starts are released. host_clock_us counts
         * whole microseconds, so a deadline 25 us ahead is at least 24 us
         * away. */
        uint64_t until = host_clock_us() + 25;
        while (host_clock_us() < until)
            ;
        AUD_PSG_REPLAY = 1;
        aud_replay(psg, 64);
        AUD_PSG_REPLAY = 0;
    }
    if (opl != 0xFFFF)
    {
        /* Writing the pointer resets the chip and empties its queue, and
         * the replay's writes queue behind it. */
        AUD_OPL_XADDR = opl;
        aud_opl_replay(opl);
    }
    aud_psg_at = psg;
    aud_opl_at = opl;
    /* The blob holds neither the PCM buffer nor its player, so an open
     * device opens again, empty. */
    AUD_PCM_CTRL = 0;
    if (aud_pcm_ctrl)
    {
        AUD_PCM_STEP = aud_pcm_step;
        AUD_PCM_CTRL = aud_pcm_ctrl;
    }
    bel_init();
}

bool psg_xreg(uint16_t word)
{
    if (word & 0x0001 || word > 0x10000 - 64 ||
        ((word >> 8) != ((word + 63) >> 8)))
    {
        AUD_PSG_XADDR = 0xFFFF;
        aud_psg_at = 0xFFFF;
        return word == 0xFFFF;
    }
    AUD_PSG_XADDR = word;
    aud_psg_at = word;
    /* While AUD_PSG_REPLAY is clear, the PSG starts or releases a voice
     * from its gate bit only on a 6502 write. AUD_PSG_REPLAY is set only
     * inside aud_restore, so this replay from the soft CPU starts no note
     * even where a gate bit in the block is set. */
    aud_replay(word, 64);
    return true;
}

bool opl_xreg(uint16_t word)
{
    if (word & 0x00FF)
    {
        AUD_OPL_XADDR = 0xFFFF;
        aud_opl_at = 0xFFFF;
        return word == 0xFFFF;
    }
    memset((void *)&XRAM_WIN[word], 0, 256);
    AUD_OPL_XADDR = word;
    aud_opl_at = word;
    return true;
}
