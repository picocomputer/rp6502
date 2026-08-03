/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * psg_xreg's validation over this machine's engine: the same rejects —
 * odd, out of bounds, or a block crossing its 256-byte page — and the
 * pointer lands in the device register, whose write also resets the
 * envelopes, the noise seeds, and the gate queue in hardware.
 *
 * Setting up either engine resets the other: the PSG is reset when an OPL
 * xreg setup is attempted, and the OPL when a PSG one is. Writing a
 * pointer register is what resets an engine, 0xFFFF included, so parking
 * the other one is the reset.
 *
 * That is the only exclusion there is. Nothing gates the mix — rp6502.sv
 * sums every engine and the bell together, and an engine with no program
 * answers zero.
 */

#include "aud.h"
#include "bel.h"
#include "mmio.h"

#include <string.h>

/* Both engines parked before anything can program them, the way the
 * RP2350's aud_init ends by setting its engines up. The platform's reset
 * is not the engines' — they hold what the last session left them, and a
 * host reset would otherwise come back to the old program still playing. */
void aud_init(void)
{
    AUD_PSG_XADDR = 0xFFFF;
    AUD_OPL_XADDR = 0xFFFF;
    bel_init();
}

/* The exit path's teardown: a stopped program's engines are reset. The
 * engines here are free-running hardware, so without this the last sound
 * plays forever. The bell is not among them — it is the soft CPU's, and a
 * rung bell rings through a program stop. */
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
    AUD_PSG_XADDR = 0xFFFF;
    AUD_OPL_XADDR = word;
    return true;
}
