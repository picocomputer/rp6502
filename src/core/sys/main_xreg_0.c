/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Device 0 of the XREG space, the RIA's own -- the one that never crosses a bus
 * even where there is one, because the registers it carries belong to the chip
 * the 6502 is talking to.
 *
 * Six rows, and every machine has all six: four human interface devices on
 * channel 0 and two sound chips on channel 1. The numbers are the 6502's ABI
 * and not any machine's choice, so they always had to agree; they were just
 * written out again per machine, in a different shape each time.
 */

#include "core/main.h"
#include "core/aud/opl.h"
#include "core/aud/psg.h"
#include "core/hid/hid.h"
#include "core/hid/keyboard.h"
#include "core/hid/mouse.h"
#include "core/hid/gamepad.h"
#include "core/hid/tablet.h"

bool main_xreg_0(uint8_t channel, uint8_t address, uint16_t word)
{
    if (channel == 0) /* human interface devices -> XRAM report blocks */
    {
        bool ok;
        switch (address)
        {
        case 0:
            ok = keyboard_xreg(word);
            break;
        case 1:
            ok = mouse_xreg(word);
            break;
        case 2:
            ok = gamepad_xreg(word);
            break;
        case 3:
            ok = tablet_xreg(word);
            break;
        default:
            return false;
        }
        hid_remapped();
        return ok;
    }
    if (channel == 1) /* audio: PSG at address 0, OPL at address 1 */
    {
        if (address == 0)
            return psg_xreg(word);
        if (address == 1)
            return opl_xreg(word);
        return false;
    }
    return false;
}
