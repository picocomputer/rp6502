/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/hid/kbd.h"
#include "emu/hid/mou.h"
#include "emu/hid/pad.h"
#include "emu/hid/tab.h"
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

bool xreg_write(uint8_t device, uint8_t channel, uint8_t address, uint16_t word)
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
        if (channel == 0 && address == 3) /* tablet -> XRAM report block */
            return tab_set_xram(word);
        if (channel == 1) /* audio: PSG at address 0, OPL at address 1 */
        {
            if (address == 0)
                return psg_xreg(word);
            if (address == 1)
                return opl_xreg(word);
            return false;
        }
        return false;

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
        /* VGA channels 1-14 go over the PIX bus to external devices with no ACK
         * required, so hardware returns success (pix_api_xreg); the emulator has
         * no such devices, so it is a no-op success. Channel 15 is the RIA-private
         * VGA control channel — normally EACCES'd in std_xreg before here, but NAK
         * it at this layer too for a direct call. */
        return channel != 0x0F;

    default: /* PIX devices 2-7: sent over the bus, no ACK, AX=0 on hardware. */
        return true;
    }
}
