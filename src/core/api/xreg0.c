/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Device 0 of the XREG space, the RIA's own. See xreg.h.
 */

#include "core/sys/driver.h"
#include "core/aud/mix.h"
#include "core/aud/opl.h"
#include "core/aud/psg.h"
#include "core/hid/hid.h"
#include "core/hid/keyboard.h"
#include "core/hid/mouse.h"
#include "core/hid/gamepad.h"
#include "core/hid/tablet.h"

bool xreg0(uint8_t channel, uint8_t index, uint16_t word)
{
    /* On channel 0 the word is the XRAM address of the device's report
     * block, or 0xFFFF to publish nothing. */
    if (channel == 0)
    {
        bool ok;
        switch (index)
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
    if (channel == 1)
    {
        /* A sound generator's block may start anywhere it ends inside
         * XRAM. Any other word parks the engine, and only 0xFFFF is not an
         * error. */
        bool ok;
        if (index == 0)
        {
            ok = psg_block_ok(word);
            aud_psg_map(ok ? word : 0xFFFF);
        }
        else if (index == 1)
        {
            ok = opl_block_ok(word);
            aud_opl_map(ok ? word : 0xFFFF);
        }
        else
            return false;
        return ok || word == 0xFFFF;
    }
    return false;
}
