/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * psg_xreg's validation over this machine's engine: the same rejects —
 * odd, out of bounds, or a block crossing its 256-byte page — and the
 * pointer lands in the device register, whose write also resets the
 * envelopes, the noise seeds, and the gate queue in hardware.
 */

#include "aud.h"
#include "com.h"
#include "mmio.h"

#include <stdio.h>

#include <string.h>

bool aud_psg_xreg(uint16_t word)
{
    if (word & 0x0001 || word > 0x10000 - 64 ||
        ((word >> 8) != ((word + 63) >> 8)))
    {
        AUD_PSG_XADDR = 0xFFFF;
        return word == 0xFFFF;
    }
    AUD_PSG_XADDR = word;
    return true;
}

/* opl_xreg's validation, which is only that the block is page-aligned:
 * the device is a 256-byte mirror of the chip's register file and an
 * offset within it is a register number, so a page is the whole of it.
 * The zeroing is opl_xreg's too — the engine reacts to writes and never
 * reads the page back, but a program that reads what it just programmed
 * should not find whatever was there before. */
bool aud_opl_xreg(uint16_t word)
{
    if (word & 0x00FF)
    {
        AUD_OPL_XADDR = 0xFFFF;
        return word == 0xFFFF;
    }
    memset((void *)&XRAM_WIN[word], 0, 256);
    AUD_OPL_XADDR = word;
    return true;
}

/* Bring-up only. The OPL2 reached hardware silent with every link of it
 * verified in simulation, and from outside the machine a snoop that
 * never fired, a chip that never ran, and a note nobody programmed all
 * look identical. This tells them apart on the debug log the Pocket
 * already carries: one line per register the snoop accepted, and the
 * tick count that says whether the chip is generating samples at all.
 *
 * Bounded at forty lines so a program that writes continuously does not
 * bury the answer. Delete this with the answer.
 */
void aud_task(void)
{
    static uint32_t seen;
    static uint8_t lines;
    uint32_t dbg = AUD_OPL_DBG;

    if (!(dbg & 0x80000000u) || dbg == seen || lines >= 40)
        return;
    seen = dbg;
    lines++;
    printf("opl: writes %u reg %02X=%02X ticks %u\n",
           (unsigned)(dbg & 0xFF), (unsigned)((dbg >> 8) & 0xFF),
           (unsigned)((dbg >> 16) & 0xFF), (unsigned)((dbg >> 24) & 0x7F));
}
