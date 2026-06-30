/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Single in-process xreg dispatcher. Replaces the PIX bus: an xreg(device,
 * channel, address, word) lands here and is routed to the RIA-local devices
 * or the VGA renderer, both of which share the one XRAM array.
 *
 * device 0 = RIA-local: channel 0 = HID (kbd/mou/pad), channel 1 = audio
 *            (PSG at address 0, OPL at address 1) — mirrors main_xreg.
 * device 1 = VGA: channel 0 holds canvas(0)/mode(1)/params, channel 15 is
 *            display control.
 */

#include "emu/hid/kbd.h"
#include "emu/hid/mou.h"
#include "emu/hid/pad.h"
#include "emu/sys/vga.h"
#include "emu/sys/xreg.h"
#include "term/term.h"
#include "modes/mode1.h"
#include "modes/mode2.h"
#include "modes/mode3.h"
#include "modes/mode4.h"
#include "modes/mode5.h"
#include "aud/psg.h"
#include "aud/opl.h"
#include <string.h>

bool emu_xreg(uint8_t device, uint8_t channel, uint8_t address, uint16_t word)
{
    switch (device)
    {
    case 0: /* RIA-local input/audio devices. */
        if (channel == 0 && address == 0) /* keyboard -> XRAM bitmap */
            return kbd_set_xram(word);
        if (channel == 0 && address == 1) /* mouse -> XRAM report block */
            return mou_set_xram(word);
        if (channel == 0 && address == 2) /* gamepad -> XRAM report block */
            return pad_set_xram(word);
        if (channel == 1) /* audio: PSG at address 0, OPL at address 1 */
        {
            if (address == 0)
                return psg_xreg(word);
            if (address == 1)
                return opl_xreg(word);
            return false;
        }
        return true; /* address 1 mouse handled above; others accepted */

    case 1: /* VGA */
        if (channel == 0)
        {
            static uint16_t xregs[16];
            xregs[address & 0x0F] = word;
            if (address == 0)
            {
                bool ok = vga_set_canvas(word);
                memset(xregs, 0, sizeof(xregs)); /* fresh state per pix.c */
                return ok;
            }
            if (address == 1)
            {
                /* Mode select (xregs[1]); params at addresses 2.. were stored
                 * first by the high->low dispatch. Mirrors vga main_prog, then
                 * clears the registers so the next program starts fresh (pix.c). */
                bool ok;
                switch (word)
                {
                case 0:
                    ok = term_prog(xregs);
                    break;
                case 1:
                    ok = mode1_prog(xregs);
                    break;
                case 2:
                    ok = mode2_prog(xregs);
                    break;
                case 3:
                    ok = mode3_prog(xregs);
                    break;
                case 4:
                    ok = mode4_prog(xregs);
                    break;
                case 5:
                    ok = mode5_prog(xregs);
                    break;
                default:
                    ok = false; /* all VGA modes modeled */
                    break;
                }
                memset(xregs, 0, sizeof(xregs));
                return ok;
            }
            return true; /* parameter register stored */
        }
        /* channel 15 is the VGA control channel: RIA-private while VGA is
         * connected, so a write NAKs (firmware API_EACCES). All other
         * channels are unmodeled and rejected. */
        return false;

    default:
        return false;
    }
}
