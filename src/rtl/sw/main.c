/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The soft CPU's runner, this platform's ria/main.c. Same scheduler:
 * drivers are notified of init, run and stop, and the API and xreg calls
 * dispatch from here.
 *
 * No break. A break drops the RIA to its monitor, and there is no monitor
 * on a Pocket — a program that wants another one execs it, and there is
 * nothing a break could drop into. Nothing can ask for one, so nothing
 * implements one.
 */

#include <stdio.h>

#include "aud.h"
#include "bel.h"
#include "cfg.h"
#include "com.h"
#include "font.h"
#include "kbd.h"
#include "main.h"
#include "mmio.h"
#include "mou.h"
#include "msc.h"
#include "pad.h"
#include "pro.h"
#include "rom.h"
#include "vga.h"
#include "vid.h"
#include "ria/api/api.h"
#include "ria/api/clk.h"
#include "ria/api/pro.h"
#include "ria/api/std.h"
#include "ria/api/tim.h"
#include "ria/api/uni.h"
#include "ria/main.h"
#include "ria/str/rln.h"
#include "ria/sys/cpu.h"
#include "ria/sys/pix.h"
#include "ria/sys/ria.h"
#include "vga/modes/mode1.h"
#include "vga/modes/mode2.h"
#include "vga/modes/mode3.h"
#include "vga/modes/mode4.h"
#include "vga/modes/mode5.h"
#include "vga/term/term.h"

#include <pico/rand.h>

#include <stdint.h>

bool ria_active(void)
{
    return false;
}

bool main_xreg_0(uint8_t channel, uint8_t address, uint16_t word)
{
    if (channel == 0 && address == 0)
        return kbd_set_xram(word);
    if (channel == 0 && address == 1)
        return mou_set_xram(word);
    if (channel == 0 && address == 2)
        return pad_set_xram(word);
    if (channel == 1 && address == 0)
        return aud_psg_xreg(word);
    if (channel == 1 && address == 1)
        return aud_opl_xreg(word);
    return false;
}

/* The VGA mode-xreg accumulator, the emulator's 16 slots. */
static uint16_t main_xregs[16];

bool main_xreg_1(uint8_t channel, uint8_t address, uint16_t word)
{
    if (channel == 0)
    {
        main_xregs[address & 0x0F] = word;
        if (address == 0)
        {
            bool ok = vga_set_canvas(word);
            for (int i = 0; i < 16; i++)
                main_xregs[i] = 0;
            return ok;
        }
        if (address == 1)
        {
            bool ok;
            vga_prog_mode((uint8_t)word, main_xregs[2]);
            switch (word)
            {
            case 0:
                ok = vid_mode0_prog(main_xregs);
                break;
            case 1:
                ok = mode1_prog(main_xregs);
                break;
            case 2:
                ok = mode2_prog(main_xregs);
                break;
            case 3:
                ok = mode3_prog(main_xregs);
                break;
            case 4:
                ok = mode4_prog(main_xregs);
                break;
            case 5:
                ok = mode5_prog(main_xregs);
                break;
            default:
                ok = false;
                break;
            }
            for (int i = 0; i < 16; i++)
                main_xregs[i] = 0;
            return ok;
        }
        return true;
    }
    if (channel == 0x0F)
    {
        /* The VGA control channel, RIA-private: pix_api_xreg refuses a
         * guest write, so these arrive only from stop(). The registers
         * with an analog on this fabric, false for the rest — the
         * emulator's rule. */
        switch (address)
        {
        case 0x00:
            /* DISPLAY, which is also the console reset. The word names a
             * display type and this machine has one, so nothing reads it. */
            vga_set_canvas(vga_canvas_console);
            term_RIS_no_clear();
            for (int i = 0; i < 16; i++)
                main_xregs[i] = 0;
            return true;
        case 0x01:
            font_set_code_page(word);
            return true;
        }
        return false;
    }
    /* Channels 1-14 reach external bus devices with no ACK; none exist,
     * so a no-op success — the emulator's rule. */
    return true;
}

/* The ROM: drive first, then the host's, which claims everything left —
 * the arrangement the shared std.c was written against, where the
 * mass-storage drive is the catch-all and comes last. */
static const std_driver_t main_drivers[] = {
    {
        .handles = rom_std_handles,
        .open = rom_std_open,
        .close = rom_std_close,
        .read = rom_std_read,
        .lseek = rom_std_lseek,
    },
    {
        .handles = msc_std_handles,
        .open = msc_std_open,
        .close = msc_std_close,
        .read = msc_std_read,
        .write = msc_std_write,
        .sync = msc_std_sync,
        .lseek = msc_std_lseek,
    },
};

const std_driver_t *main_std_drivers(size_t *count)
{
    *count = sizeof main_drivers / sizeof main_drivers[0];
    return main_drivers;
}

bool main_api(uint8_t operation)
{
    switch (operation)
    {
    case 0x01:
        return pix_api_xreg();
    case 0x02:
        /* atr_api_phi2's shape: a report, not a control. Attribute
         * 0x01 is what sets it. */
        return api_return_ax(cpu_get_phi2_khz_run());
    case 0x03:
        /* atr_api_code_page's shape: the glyphs. The conversion tables
         * are a separate asset on a separate slot and fail separately,
         * so a font page is not refused on their account. A page this
         * machine does not carry is a no-op — 0 is not a page, so the
         * guard is also the old non-zero test — and the get that
         * follows says which page is actually in force. */
        if (font_has_code_page(API_AX))
            font_set_code_page(API_AX);
        return api_return_ax(font_get_code_page());
    case 0x04:
        return api_return_axsreg(get_rand_64() & 0x7FFFFFFF);
    case 0x06:
        if (!api_set_errno_opt(API_A))
            return api_return_errno(API_EINVAL);
        return api_return_ax(0);
    case 0x08:
        return pro_api_argv();
    case 0x09:
        return pro_api_exec();
    case 0x0A:
        switch (API_A)
        {
        case 0x00:
            return api_return_axsreg(api_get_errno_opt());
        case 0x01:
            return api_return_axsreg(cpu_get_phi2_khz_run());
        case 0x02:
            return api_return_axsreg(font_get_code_page());
        case 0x03:
            return api_return_axsreg(rln_get_max_length());
        case 0x04:
            return api_return_axsreg(get_rand_64() & 0x7FFFFFFF);
        case 0x05:
            return api_return_axsreg(com_get_bel());
        case 0x09:
            return api_return_axsreg(rln_get_caps());
        case 0x0A:
            return api_return_axsreg(rln_get_term_width());
        case 0x0B:
            return api_return_axsreg(rln_get_term_height());
        case 0x0C:
            return api_return_axsreg(rln_get_suppress_nl());
        case 0x10:
            return api_return_axsreg(clk_get_run(1000) & 0x7FFFFFFF);
        case 0x11:
            return api_return_axsreg(clk_get_run(10000) & 0x7FFFFFFF);
        case 0x12:
            return api_return_axsreg(clk_get_run(100000) & 0x7FFFFFFF);
        case 0x13:
            return api_return_axsreg(clk_get_run(1000000) & 0x7FFFFFFF);
        default:
            return api_return_errno(API_EINVAL);
        }
    case 0x0B:
    {
        uint32_t value;
        if (!api_pop_uint32_end(&value))
            return api_return_errno(API_EINVAL);
        if (value > 0x7FFFFFFF)
            return api_return_errno(API_EINVAL);
        switch (API_A)
        {
        case 0x00:
            if (value > UINT8_MAX || !api_set_errno_opt((uint8_t)value))
                return api_return_errno(API_EINVAL);
            break;
        case 0x01:
            if (value < CPU_PHI2_MIN_KHZ || value > CPU_PHI2_MAX_KHZ)
                return api_return_errno(API_EINVAL);
            cpu_set_phi2_khz_run((uint16_t)value);
            break;
        case 0x02:
            /* oem_set_code_page_run's shape, which is a void: a page
             * f_setcp will not take leaves the RIA's page where it was
             * and still answers the set with success. The seventeen
             * FatFs carries there are the seventeen this asset carries
             * here, so the same request is refused the same way. */
            if (value > UINT16_MAX)
                return api_return_errno(API_EINVAL);
            if (font_has_code_page((uint16_t)value))
                font_set_code_page((uint16_t)value);
            break;
        case 0x03:
            if (value > UINT8_MAX)
                return api_return_errno(API_EINVAL);
            rln_set_max_length((uint8_t)value);
            break;
        case 0x05:
            if (value > 1)
                return api_return_errno(API_EINVAL);
            com_set_bel(value);
            break;
        case 0x09:
            if (value > 2)
                return api_return_errno(API_EINVAL);
            rln_set_caps((uint8_t)value);
            break;
        case 0x0A:
            if (value > UINT16_MAX)
                return api_return_errno(API_EINVAL);
            rln_set_term_width((uint16_t)value);
            break;
        case 0x0B:
            if (value > UINT16_MAX)
                return api_return_errno(API_EINVAL);
            rln_set_term_height((uint16_t)value);
            break;
        case 0x0C:
            if (value > 1)
                return api_return_errno(API_EINVAL);
            rln_set_suppress_nl((uint8_t)value);
            break;
        default:
            return api_return_errno(API_EINVAL);
        }
        return api_return_ax(0);
    }
    case 0x14:
        return std_api_open();
    case 0x15:
        return std_api_close();
    case 0x16:
        return std_api_read_xstack();
    case 0x17:
        return std_api_read_xram();
    case 0x18:
        return std_api_write_xstack();
    case 0x19:
        return std_api_write_xram();
    case 0x0F:
        return clk_api_clock();
    case 0x10:
        return clk_api_get_res();
    case 0x11:
        return clk_api_get_time();
    case 0x12:
        return clk_api_set_time();
    case 0x1A:
        return std_api_lseek_cc65();
    case 0x1D:
        return std_api_lseek_llvm();
    case 0x1E:
        return std_api_syncfs();
    case 0x29:
        return msc_api_chdir();
    case 0x2A:
        return msc_api_chdrive();
    case 0x2B:
        return msc_api_getcwd();
    case 0x30:
        return rln_api_lastkey();
    case 0x31:
        return rln_api_peek();
    case 0x32:
        return rln_api_poke();
    case 0x3A:
        return clk_api_gmtime();
    case 0x3B:
        return clk_api_localtime();
    case 0x3C:
        return clk_api_mktime();
    case 0x3D:
        return clk_api_strftime();
    case 0x3E:
        return clk_api_time_set();
    case 0x3F:
        return clk_api_time_get();
    default:
        /* What lands here now is the directory family — the drive has
         * one directory and nothing to enumerate — plus unlink, rename
         * and stat, which the host's API cannot express. */
        return api_return_errno(API_ENOSYS);
    }
}

/* Power-up, once. No str_init: it exists to apply a locale, and this
 * machine has one locale and no S() callers — the whole localized chain
 * is meant to collect under --gc-sections. */
static void init(void)
{
    cpu_init();
    aud_init();
    com_init();
    std_init();
    rln_init();
    term_init();
    /* The code page tables came in on their own data slot. Say so if
     * they did not: the machine runs either way, and the alternative to
     * saying so is a program whose accented filenames quietly stop
     * matching. Halting over a text table would be the worse trade. */
    if (!uni_init())
        printf("oem: no tables\n");
    vid_init();
    tim_init();
}

/* The 6502 coming out of reset. */
static void run(void)
{
    com_run();
    rln_run();
    api_run();
    clk_run();
    cpu_run(); /* Must be last: this is RESB going high. */
}

/* The 6502 going into reset. Anything a program left running is the
 * firmware's to put back, because none of it is on the platform's reset
 * — the engines would otherwise play the last note forever. */
static void stop(void)
{
    cpu_stop(); /* Must be first. */
    rln_stop();
    api_stop();
    std_stop();
    msc_stop(); /* after std_stop: its closes are what park a read */
    kbd_stop();
    mou_stop();
    pad_stop();
    aud_stop();
    /* No pro_stop: argv belongs to the image, not the run. An exec is
     * the one thing that replaces it, and it brings its own before the
     * stop it asks for. */
    /* The VGA control channel last, where the RIA's deferred vga_task
     * puts it. Two registers: the code page is oem_stop's job there. */
    main_xreg_1(0x0F, 0x01, 437);
    main_xreg_1(0x0F, 0x00, vga_get_display_type());
}

/* The host announced a slot while we were running, applied at the stop.
 *
 * The ROM slot is user-reloadable and does not ask for a reset: with
 * bit 6 of its parameters clear, APF rewrites the image at the slot's
 * address and sends command 0x008A alone — no request write, no
 * access-all-complete, no Reset Enter and Exit — so the machine keeps
 * running and this is the only news it gets. Bit 6 set would restart
 * the firmware instead, and take the terminal with it.
 *
 * A count rather than an event, incremented on the host's own clock
 * where nothing between here and there can drop one. What the new image
 * is remains a question for the data table, which is where the loader
 * asks it anyway. */
static uint8_t main_upd_seen;

static bool main_rom_len(uint32_t *len)
{
    return msc_slot_len(MSC_SLOT_ROM, len) && *len;
}

static enum state {
    stopped,
    starting,
    running,
    stopping,
} volatile main_state;

/* Callers belong in the loop's stopped block and nowhere else: promoted
 * out of stopping, this would skip the outgoing program's teardown. */
void main_run(void)
{
    if (main_state != running)
        main_state = starting;
}

void main_stop(void)
{
    cpu_stop(); /* RESB down now; the rest of the fan-out can wait. */
    if (main_state == starting)
        main_state = stopped;
    else if (main_state != stopped)
        main_state = stopping;
}

bool main_active(void)
{
    return main_state != stopped;
}

/* The loader parses the image straight out of the platform's staging
 * store. data.json marks the ROM slot required, so the host will not
 * launch this core without one and a missing length means a platform
 * that staged nothing — which leaves nothing to run and nothing to say. */
static void main_stage(void)
{
    uint32_t len;
    bool staged = main_rom_len(&len);
    bool ok = staged && rom_load_staged(len);
    /* After the load, not before. At boot the core is already running
     * when the host is still staging, and the data table answers from
     * the bridge while Get File needs the host: asked first, it waits
     * out the bridge's whole deadline and the machine never starts. */
    pro_restage();
    /* Cleared once the image is dealt with and not before: it is how
     * anything watching tells a load in progress from a finished one. */
    MMIO_SLOT = 0;
    if (ok)
        main_run();
    else if (staged)
        printf("rom: bad image\n");
}

int main(void)
{
    init();

    main_stage();
    /* Whatever the host has announced up to here is this image. */
    main_upd_seen = (uint8_t)MMIO_UPD_N;

    /* The OS loop, in the firmware's task order with api last. The real
     * api.c latches the op and dispatches through main_api; the
     * manifold moves the console bytes. It never ends: a machine's
     * firmware has nowhere to return to, and the simulation decides
     * for itself when a run is over. */
    for (;;)
    {
        /* Loop-local: main_stop() leaves stopping or stopped, and both
         * are handled below, so the news cannot outlive the pass that
         * took it. A static one that missed its branch would strand the
         * machine, which has no monitor to recover into. */
        bool restage = false;

        if (API_PENDING)
        {
            /* ZXSTACK and EXIT run inside the $FFEF write on the RIA
             * and the emulator; here the pending strobe is that write. */
            API_PENDING = 0;
            if (API_OP == 0x00)
            {
                xstack_ptr = XSTACK_SIZE;
                api_return_ax(0);
            }
            else if (API_OP == 0xFF)
            {
                main_stop();
                api_return_ax(0);
            }
        }
        cfg_task();
        kbd_task();
        pad_task();
        mou_task();
        std_task();
        com_task();
        bel_task();
        rln_task();
        term_task();
        vid_task();
        api_task();
        uint8_t upd = (uint8_t)MMIO_UPD_N;
        if (upd != main_upd_seen)
        {
            main_upd_seen = upd;
            restage = true;
            main_stop();
        }
        if (main_state == starting)
        {
            run();
            main_state = running;
        }
        if (main_state == stopping)
        {
            stop();
            main_state = stopped;
        }
        /* Below both branches rather than inside stopping, because a
         * machine already stopped is owed no stop and would otherwise
         * never launch: a program that exited, an exec that failed, or a
         * pick that landed before the last one had begun to run. */
        if (main_state == stopped)
        {
            if (restage)
            {
                /* The RIA ends every stop with com_stop's soft reset. This
                 * host has no such thing, and a program the user replaced
                 * from the menu would otherwise run its first line into
                 * the last line of the one before. An exec gets nothing:
                 * the outgoing program chose its successor and the two
                 * read as one session. */
                com_putchar('\n');
                main_stage();
            }
            else if (pro_exec_take())
                main_run();
        }
    }
}
