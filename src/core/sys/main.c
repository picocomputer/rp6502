/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "core/sys/main.h"
#include "core/sys/pro.h"
#include "core/aud/aud_mix.h"
#include "core/dap/dbg.h"
#include "core/sys/msc.h"
#include "core/sys/rom.h"
#include "core/com/com.h"
#include "core/wdc/cpu.h"
#include "core/mem/mem.h"
#include "core/pix.h"
#include "core/vga/vga_emu.h"
#include "core/wdc/via.h"
#include "core/hid/kbd.h"
#include "core/hid/mou.h"
#include "core/hid/pad.h"
#include "core/hid/tab.h"
#include "core/ria/ria.h"
#include "core/api/api.h"
#include "core/api/atr.h"
#include "core/api/dir.h"
#include "core/api/std.h"
#include "core/api/clk.h"
#include "core/api/oem.h"
#include "core/api/tim.h"
#include "core/aud/aud.h"
#include "core/aud/psg.h"
#include "core/aud/opl.h"
#include "core/str/rln.h"
#include "core/str/str.h"
#include "core/term/font.h"
#include "core/term/term.h"
#include "core/vga/mode0.h"
#include "core/vga/mode1.h"
#include "core/vga/mode2.h"
#include "core/vga/mode3.h"
#include "core/vga/mode4.h"
#include "core/vga/mode5.h"
#include <stdio.h>
#include <string.h>

void main_init(void)
{
    mem_init();
    pro_init();
    cpu_init();
    aud_init();
    kbd_init();
    mou_init();
    pad_init();
    tab_init();
    com_init();
    std_init();
    rln_init();
    tim_init();
    str_init();
    oem_init();
    font_init();
    term_init();
    vga_init();
}

void main_run(void)
{
    pro_run();
    com_run();
    rln_run();
    dir_run();
    api_run();
    clk_run();
    ria_run();
    via_run();
    cpu_run(); /* must be last */
}

void main_stop(void)
{
    cpu_stop(); /* must be first */
    vga_stop();
    rln_stop();
    api_stop();
    oem_stop(); /* a run-only code page belongs to the run that set it */
    std_stop();
    dir_stop();
    kbd_stop();
    mou_stop();
    pad_stop();
    tab_stop();
    aud_stop();
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

/* PIX XREG register dispatch. Device 0 is the RIA-local virtual device (HID +
 * audio); device 1 is the VGA. False on an unhandled channel/address. */
bool main_xreg_0(uint8_t channel, uint8_t address, uint16_t word)
{
    if (channel == 0) /* human interface devices -> XRAM report blocks */
    {
        if (address == 0)
            return kbd_xreg(word);
        if (address == 1)
            return mou_xreg(word);
        if (address == 2)
            return pad_xreg(word);
        if (address == 3)
            return tab_xreg(word);
        return false;
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

/* The VGA mode-xreg accumulator, shared by channel 0 (CANVAS/MODE) and channel 15
 * (DISPLAY, which clears it) — mirrors the file-level xregs in vga/sys/pix.c. */
static uint16_t xregs[16];

bool main_xreg_1(uint8_t channel, uint8_t address, uint16_t word)
{
    if (channel == 0)
    {
        xregs[address & 0x0F] = word;
        if (address == 0)
        {
            bool ok = vga_set_canvas(word);
            memset(xregs, 0, sizeof(xregs)); /* fresh state per pix.c */
            return ok;
        }
        if (address == 1)
        {
            /* Mode select (xregs[1]); params at addresses 2.. were stored first
             * by the high->low dispatch. Mirrors vga main_prog, then clears the
             * registers so the next program starts fresh. */
            bool ok = vga_mode_prog(word, xregs);
            memset(xregs, 0, sizeof(xregs));
            return ok;
        }
        return true; /* parameter register stored */
    }
    if (channel == 0x0F)
    {
        /* VGA control channel — RIA-private (guest writes NAK in pix_api_xreg).
         * Mirrors vga/sys/pix.c pix_ch15_xreg; only registers with an emu analog
         * are handled. */
        if (address == 0x00) /* DISPLAY: also resets to the console canvas */
        {
            vga_set_canvas(vga_canvas_console); /* == firmware vga_xreg_canvas(NULL) */
            term_RIS_no_clear();                /* preserve-screen terminal RIS */
            memset(xregs, 0, sizeof(xregs));
            return true;
        }
        return false; /* no emu analog for the other control registers */
    }
    /* Channels 1-14 reach external bus devices with no ACK; the emulator has none,
     * so a no-op success. */
    return true;
}

const dir_backend_t *main_dir_backend(void)
{
    return &msc_dir_backend;
}

static const std_driver_t std_drivers[] = {
    {rom_std_handles, rom_std_open, rom_std_close, rom_std_read, NULL, NULL, rom_std_lseek},
    {msc_std_handles, msc_std_open, msc_std_close, msc_std_read, msc_std_write, msc_std_sync, msc_std_lseek},
};

const std_driver_t *main_std_drivers(size_t *count)
{
    *count = sizeof(std_drivers) / sizeof(std_drivers[0]);
    return std_drivers;
}
