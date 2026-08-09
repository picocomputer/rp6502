/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * psg_xreg's validation over this machine's engine. Writing a pointer
 * register resets that engine's envelopes, noise seeds and gates in
 * hardware, 0xFFFF included.
 *
 * Setting up either engine parks the other, which is the only exclusion
 * there is: nothing gates the mix, and rp6502.sv sums every engine and
 * the bell together.
 */

#include "aud.h"
#include "bel.h"
#include "mmio.h"

#include <string.h>

/* The platform's reset is not the engines': they hold what the last
 * session left them, so a host reset would come back still playing. */
void aud_init(void)
{
    AUD_PSG_XADDR = 0xFFFF;
    AUD_OPL_XADDR = 0xFFFF;
    bel_init();
}

/* Free-running hardware, so without this the last sound plays forever.
 * The bell is the soft CPU's and rings through a program stop. */
void aud_stop(void)
{
    AUD_PSG_XADDR = 0xFFFF;
    AUD_OPL_XADDR = 0xFFFF;
}

bool aud_psg_xreg(uint16_t word)
{
    if (word & 0x0001 || word > 0x10000 - 64 ||
        ((word >> 8) != ((word + 63) >> 8)))
    {
        AUD_PSG_XADDR = 0xFFFF;
        return word == 0xFFFF;
    }
    AUD_OPL_XADDR = 0xFFFF;
    AUD_PSG_XADDR = word;
    /* The engine learns from writes and never reads the block back, so a
     * block programmed before the pointer would be invisible. Writing
     * each byte over itself is that block arriving. From here and not the
     * 6502: only the 6502's writes strike gates. */
    for (uint8_t i = 0; i < 64; i++)
    {
        uint8_t v = XRAM_WIN[word + i];
        XRAM_WIN[word + i] = v;
    }
    return true;
}

/* Page-aligned is the whole validation: the device is a 256-byte mirror
 * of the chip's register file. The zeroing is for a program that reads
 * back what it just programmed; the engine never reads the page. */
bool aud_opl_xreg(uint16_t word)
{
    if (word & 0x00FF)
    {
        AUD_OPL_XADDR = 0xFFFF;
        return word == 0xFFFF;
    }
    memset((void *)&XRAM_WIN[word], 0, 256);
    AUD_PSG_XADDR = 0xFFFF;
    AUD_OPL_XADDR = word;
    return true;
}
