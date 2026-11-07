/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mmio.h"

#include "core/aud/bel.h"
#include "core/aud/mix.h"

/* The open PCM: device's AUD_PCM_CTRL, 0 while it is closed. */
static uint32_t aud_pcm_ctrl;
/* Bytes written since the open, which empties the FIFO, modulo 4. */
static uint8_t aud_pcm_head;

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

/* The bell waits out a wake, until wake_task has moved its deadlines. */
void aud_task(void)
{
    if (!(SST_CTL & SST_RESTORED))
        bel_task();
}

void aud_bel_put(unsigned i, uint8_t val) { AUD_BEL = i << 8 | val; }

/* A pointer write zeroes the PSG's eight voices' registers in a pass of
 * about 64 clocks, which a write of the bell's or a program's would race,
 * so nothing goes on until it is done. */
void aud_psg_map(uint16_t word)
{
    AUD_PSG_XADDR = word;
    while (AUD_PSG_BUSY)
        ;
}

/* The OPL holds writes in its queue while it clears its registers. */
void aud_opl_map(uint16_t word) { AUD_OPL_XADDR = word; }

/* A program's end takes its sound with it, so the close that std_stop makes
 * next has nothing to wait for. */
void aud_stop(void)
{
    aud_psg_map(0xFFFF);
    aud_opl_map(0xFFFF);
    AUD_PCM_CLEAR = 0;
    /* A read clears the underrun count. */
    (void)AUD_PCM_UNDERRUNS;
}

bool aud_pcm_open(const pcm_fmt_t *fmt)
{
    if (aud_pcm_ctrl)
        return false;
    aud_pcm_ctrl = 1u | (fmt->bits == 16 ? 2u : 0u) | (fmt->channels == 2 ? 4u : 0u);
    AUD_PCM_STEP = (uint32_t)(pcm_step(fmt->rate) - 1);
    AUD_PCM_CTRL = aud_pcm_ctrl;
    aud_pcm_head = 0;
    return true;
}

void aud_pcm_close(void)
{
    aud_pcm_ctrl = 0;
    AUD_PCM_CTRL = 0;
}

/* The player only takes, so the room read first is never too much. buf is
 * in XRAM or the xstack, and a word load from XRAM gives one byte four
 * times, so each byte is a load of its own. A word lands on a word of the
 * FIFO, so bytes go one at a time, in every lane, until the write pointer
 * is on one; aud_pcm_head counts its place in its word. The commit makes
 * the write playable whole, as on the other machines. */
uint32_t aud_pcm_write(const uint8_t *buf, uint32_t n)
{
    const volatile uint8_t *src = buf;
    const uint32_t room = PCM_BUF_SIZE - AUD_PCM_LEVEL;
    if (n > room)
        n = room;
    uint32_t i = 0;
    for (; i < n && (aud_pcm_head & 3); i++, aud_pcm_head++)
        AUD_PCM_BYTE = src[i] * 0x01010101u;
    for (; i + 4 <= n; i += 4)
        AUD_PCM_DATA = (uint32_t)src[i] | (uint32_t)src[i + 1] << 8 |
                       (uint32_t)src[i + 2] << 16 | (uint32_t)src[i + 3] << 24;
    for (; i < n; i++, aud_pcm_head++)
        AUD_PCM_BYTE = src[i] * 0x01010101u;
    AUD_PCM_COMMIT = 0;
    return n;
}

uint16_t aud_pcm_level(void) { return (uint16_t)AUD_PCM_LEVEL; }

void aud_pcm_clear(void) { AUD_PCM_CLEAR = 0; }

uint32_t aud_opl_overruns(void) { return AUD_OPL_OVERRUNS; }

uint32_t aud_pcm_underruns(void) { return AUD_PCM_UNDERRUNS; }
