/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/main.h"
#include "emu/emu/pro.h"
#include "emu/emu/aud.h"
#include "emu/dbg/dbg.h"
#include "emu/emu/msc.h"
#include "emu/emu/rom.h"
#include "emu/emu/tmp.h"
#include "emu/sys/com.h"
#include "emu/sys/cpu.h"
#include "emu/sys/mem.h"
#include "ria/sys/pix.h"
#include "emu/sys/vga.h"
#include "emu/emu/via.h"
#include "emu/hid/kbd.h"
#include "emu/hid/mou.h"
#include "emu/hid/pad.h"
#include "emu/hid/tab.h"
#include "emu/sys/ria.h"
#include "ria/api/api.h"
#include "ria/api/atr.h"
#include "ria/api/std.h"
#include "ria/api/fat.h"
#include "ria/api/clk.h"
#include "ria/api/oem.h"
#include "ria/api/tim.h"
#include "ria/aud/aud.h"
#include "ria/aud/psg.h"
#include "ria/aud/opl.h"
#include "ria/str/rln.h"
#include "ria/str/str.h"
#include "ria/sys/sys.h"
#include "vga/term/font.h"
#include "vga/term/term.h"
#include "vga/modes/mode1.h"
#include "vga/modes/mode2.h"
#include "vga/modes/mode3.h"
#include "vga/modes/mode4.h"
#include "vga/modes/mode5.h"
#include <stdio.h>
#include <string.h>

void main_init(void)
{
    pro_init();
    cpu_init();
    aud_init();
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
    fat_run();
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
    std_stop();
    fat_stop();
    msc_stop();
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
            return kbd_set_xram(word);
        if (address == 1)
            return mou_set_xram(word);
        if (address == 2)
            return pad_set_xram(word);
        if (address == 3)
            return tab_set_xram(word);
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

/* The 6502 syscall op -> handler table. A runtime array (not a switch) so the dir
 * slots can be swapped between the emu's host handlers and the REAL firmware
 * fat_api_* (ria/api/fat.c) when --tmpdrive mounts a RAM FatFs. The dir slots
 * default to host below; main_dir_ops_set() swaps them. */
typedef bool (*api_op_fn)(void);
static api_op_fn api_ops[0x40] = {
    [0x01] = pix_api_xreg,
    [0x02] = atr_api_phi2,
    [0x03] = atr_api_code_page,
    [0x04] = atr_api_lrand,
    [0x06] = atr_api_errno_opt,
    [0x08] = pro_api_argv,
    [0x09] = pro_api_exec,
    [0x0A] = atr_api_get,
    [0x0B] = atr_api_set,
    [0x0F] = clk_api_clock,
    [0x10] = clk_api_get_res,
    [0x11] = clk_api_get_time,
    [0x12] = clk_api_set_time,
    [0x14] = std_api_open,
    [0x15] = std_api_close,
    [0x16] = std_api_read_xstack,
    [0x17] = std_api_read_xram,
    [0x18] = std_api_write_xstack,
    [0x19] = std_api_write_xram,
    [0x1A] = std_api_lseek_cc65,
    [0x1B] = msc_api_unlink,
    [0x1C] = msc_api_rename,
    [0x1D] = std_api_lseek_llvm,
    [0x1E] = std_api_syncfs,
    [0x1F] = msc_api_stat,
    [0x20] = msc_api_opendir,
    [0x21] = msc_api_readdir,
    [0x22] = msc_api_closedir,
    [0x23] = msc_api_telldir,
    [0x24] = msc_api_seekdir,
    [0x25] = msc_api_rewinddir,
    [0x26] = msc_api_chmod,
    [0x27] = msc_api_utime,
    [0x28] = msc_api_mkdir,
    [0x29] = msc_api_chdir,
    [0x2A] = msc_api_chdrive,
    [0x2B] = msc_api_getcwd,
    [0x2C] = msc_api_setlabel,
    [0x2D] = msc_api_getlabel,
    [0x2E] = msc_api_getfree,
    [0x30] = rln_api_lastkey,
    [0x31] = rln_api_peek,
    [0x32] = rln_api_poke,
    [0x3A] = clk_api_gmtime,
    [0x3B] = clk_api_localtime,
    [0x3C] = clk_api_mktime,
    [0x3D] = clk_api_strftime,
    [0x3E] = clk_api_time_set,
    [0x3F] = clk_api_time_get,
};

/* Swap the dir op slots: the firmware's own fat_api_* (over the RAM FatFs) when
 * fat, else the emu's host handlers. */
void main_dir_ops_set(bool fat)
{
    static const struct
    {
        uint8_t op;
        api_op_fn host, fat;
    } slots[] = {
        {0x1B, msc_api_unlink, fat_api_unlink},
        {0x1C, msc_api_rename, fat_api_rename},
        {0x1F, msc_api_stat, fat_api_stat},
        {0x20, msc_api_opendir, fat_api_opendir},
        {0x21, msc_api_readdir, fat_api_readdir},
        {0x22, msc_api_closedir, fat_api_closedir},
        {0x23, msc_api_telldir, fat_api_telldir},
        {0x24, msc_api_seekdir, fat_api_seekdir},
        {0x25, msc_api_rewinddir, fat_api_rewinddir},
        {0x26, msc_api_chmod, fat_api_chmod},
        {0x27, msc_api_utime, fat_api_utime},
        {0x28, msc_api_mkdir, fat_api_mkdir},
        {0x29, msc_api_chdir, fat_api_chdir},
        {0x2A, msc_api_chdrive, fat_api_chdrive},
        {0x2B, msc_api_getcwd, fat_api_getcwd},
        {0x2C, msc_api_setlabel, fat_api_setlabel},
        {0x2D, msc_api_getlabel, fat_api_getlabel},
        {0x2E, msc_api_getfree, fat_api_getfree},
    };
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++)
        api_ops[slots[i].op] = fat ? slots[i].fat : slots[i].host;
}

/* The registry api_task dispatches through; the firmware's is the switch in
 * main.c. Returns true if the op has more work (api_working) and should be
 * re-dispatched, false once it has returned to the 6502. */
bool main_api(uint8_t operation)
{
    api_op_fn fn = operation < 0x40 ? api_ops[operation] : NULL;
    return fn ? fn() : api_return_errno(API_ENOSYS);
}

static const std_driver_t std_drivers[] = {
    {rom_std_handles, rom_std_open, rom_std_close, rom_std_read, NULL, NULL, rom_std_lseek},
    {tmp_std_handles, fat_std_open, fat_std_close, fat_std_read, fat_std_write, fat_std_sync, fat_std_lseek},
    {msc_std_handles, msc_std_open, msc_std_close, msc_std_read, msc_std_write, msc_std_sync, msc_std_lseek},
};

const std_driver_t *main_std_drivers(size_t *count)
{
    *count = sizeof(std_drivers) / sizeof(std_drivers[0]);
    return std_drivers;
}
