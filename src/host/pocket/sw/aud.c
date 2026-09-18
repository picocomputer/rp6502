/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "aud.h"
#include "bel.h"
#include "mmio.h"

#include "host/host.h"


#include <string.h>

/* The pointer registers cannot be read back and the savestate does not
 * include them, so each pointer is also kept here in the TCM, which the
 * savestate does include. */
static uint16_t aud_psg_at = 0xFFFF;
static uint16_t aud_opl_at = 0xFFFF;


/* The PSG, the OPL and the bell voice have no reset input, so they keep
 * sounding through a machine reset until their registers are written.
 * Writing the OPL's pointer register, 0xFFFF included, silences the OPL,
 * and writing the PSG's silences its first eight voices. The bell voice is
 * the PSG's ninth voice and a pointer write does not silence it, so
 * bel_init clears its registers. */
void aud_init(void)
{
    aud_stop();
    bel_init();
}

void aud_stop(void)
{
    AUD_PSG_XADDR = 0xFFFF;
    AUD_OPL_XADDR = 0xFFFF;
    aud_psg_at = 0xFFFF;
    aud_opl_at = 0xFFFF;
}

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
    else if (opl != 0xFFFF)
    {
        AUD_OPL_XADDR = opl;
        /* Writing the pointer also resets the chip, and opl.sv holds
         * that reset for 255 clocks and drops every register write that
         * arrives during it. 255 clocks at 50.4 MHz is 5.06 us.
         * host_clock_us counts whole microseconds, so a deadline N ahead
         * is only certain to be N - 1 us away, and 7 is the smallest N
         * for which N - 1 exceeds 5.06. */
        uint64_t until = host_clock_us() + 7;
        while (host_clock_us() < until)
            ;
        aud_replay(opl, 256);
    }
    aud_psg_at = psg;
    aud_opl_at = opl;
    bel_init();
}

bool psg_xreg(uint16_t word)
{
    if (word & 0x0001 || word > 0x10000 - 64 ||
        ((word >> 8) != ((word + 63) >> 8)))
    {
        aud_stop();
        return word == 0xFFFF;
    }
    AUD_OPL_XADDR = 0xFFFF;
    AUD_PSG_XADDR = word;
    aud_opl_at = 0xFFFF;
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
        aud_stop();
        return word == 0xFFFF;
    }
    memset((void *)&XRAM_WIN[word], 0, 256);
    AUD_PSG_XADDR = 0xFFFF;
    AUD_OPL_XADDR = word;
    aud_psg_at = 0xFFFF;
    aud_opl_at = word;
    return true;
}
