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
#include "mmio.h"

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
