/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Which device an XREG write reaches: device 0, the RIA's own -- the one that
 * never crosses a bus even where there is one, because the registers it
 * carries belong to the chip the 6502 is talking to -- and device 1, the
 * video device, on a machine that is its own.
 *
 * The rows are the 6502's ABI and not any machine's choice, so they always had
 * to agree; they were just written out again per machine, in a different shape
 * each time. A machine whose video device is a real chip across four wires
 * answers device 1 at the far end instead (host/pico/vga/sys/pix.c).
 */

#include "core/main.h"
#include "core/aud/opl.h"
#include "core/aud/psg.h"
#include "core/hid/hid.h"
#include "core/hid/kbd.h"
#include "core/hid/mou.h"
#include "core/hid/pad.h"
#include "core/hid/tab.h"
#include "core/term/term.h"
#include "core/vga/vga.h"

#include <string.h>

bool main_xreg_0(uint8_t channel, uint8_t address, uint16_t word)
{
    if (channel == 0) /* human interface devices -> XRAM report blocks */
    {
        bool ok;
        switch (address)
        {
        case 0:
            ok = kbd_xreg(word);
            break;
        case 1:
            ok = mou_xreg(word);
            break;
        case 2:
            ok = pad_xreg(word);
            break;
        case 3:
            ok = tab_xreg(word);
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

/* The mode program being assembled. Channel 0 stores each register as it
 * arrives and the mode write consumes the lot; the dispatch in core/sys/pix.c
 * sends them high address to low, so the parameters are here before the mode
 * that reads them. */
static uint16_t xregs[16];

static void xregs_clear(void)
{
    memset(xregs, 0, sizeof(xregs));
}

bool main_xreg_1(uint8_t channel, uint8_t address, uint16_t word)
{
    if (channel == 0)
    {
        xregs[address & 0x0F] = word;
        if (address == 0) /* CANVAS: a new one starts with no programming */
        {
            bool ok = vga_canvas_select(word);
            xregs_clear();
            return ok;
        }
        if (address == 1) /* MODE: consumes what came before it */
        {
            vga_mode_begin((uint8_t)word, xregs[2]);
            bool ok = vga_mode_prog(word, xregs);
            xregs_clear();
            return ok;
        }
        return true; /* a parameter register, stored */
    }
    if (channel == 0x0F)
    {
        /* The control channel is the RIA's, not a program's -- pix_api_xreg
         * refuses a guest write -- so these arrive only from the machine. */
        switch (address)
        {
        case 0x00: /* DISPLAY, which is also the reset to the console */
            vga_canvas_select(vga_canvas_console);
            term_RIS_no_clear(); /* the screen survives the reset */
            xregs_clear();
            return true;
        case 0x01: /* CODE_PAGE */
            vga_set_code_page(word);
            return true;
        }
        return false;
    }
    /* Channels 1-14 reach bus devices with no ACK, and there is no bus. */
    return true;
}
