/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The soft CPU's runner, this platform's ria/main.c.
 */

#include <stdio.h>

#include "apf.h"
#include "aud.h"
#include "bel.h"
#include "cfg.h"
#include "com.h"
#include "font.h"
#include "log.h"
#include "main.h"
#include "mmio.h"
#include "msc.h"
#include "pro.h"
#include "rand.h"
#include "rom.h"
#include "sst.h"
#include "vga.h"
#include "vid.h"
#include "core/api/api.h"
#include "core/api/clk.h"
#include "core/api/pro.h"
#include "core/api/std.h"
#include "core/api/tim.h"
#include "core/api/uni.h"
#include "core/hid/kbd.h"
#include "core/hid/kbt.h"
#include "core/hid/kbl.h"
#include "core/hid/mou.h"
#include "core/hid/pad.h"
#include "core/hid/tab.h"
#include "host/pico/ria/main.h"
#include "core/str/rln.h"
#include "host/pico/ria/sys/cpu.h"
#include "core/pix.h"
#include "host/pico/ria/sys/ria.h"
#include "core/vga/mode1.h"
#include "core/vga/mode2.h"
#include "core/vga/mode3.h"
#include "core/vga/mode4.h"
#include "core/vga/mode5.h"
#include "core/term/term.h"

#include "host.h"

#include <stdint.h>

bool ria_active(void)
{
    return false;
}

void ria_trigger_sigint(void)
{
}

bool main_xreg_0(uint8_t channel, uint8_t address, uint16_t word)
{
    if (channel == 0 && address <= 3)
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
        default:
            ok = tab_xreg(word);
            break;
        }
        /* Mapping blanks the record; reports only arrive on movement, so
         * the next one has to count as news. */
        apf_refresh();
        return ok;
    }
    if (channel == 1 && address == 0)
        return aud_psg_xreg(word);
    if (channel == 1 && address == 1)
        return aud_opl_xreg(word);
    return false;
}

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
        /* RIA-private: pix_api_xreg refuses a guest write, so these
         * arrive only from stop(). */
        switch (address)
        {
        case 0x00:
            /* DISPLAY, which is also the console reset. */
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
    /* Channels 1-14 reach external bus devices with no ACK; none exist. */
    return true;
}

/* std.c wants the catch-all last, so the mass-storage drive follows ROM. */
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
        return api_return_ax(cpu_get_phi2_khz_run());
    case 0x03:
        /* A page this machine does not carry is a no-op; the get that
         * follows says which page is actually in force. */
        if (font_has_code_page(API_AX))
            font_set_code_page(API_AX);
        return api_return_ax(font_get_code_page());
    case 0x04:
        return api_return_axsreg(host_rand_64() & 0x7FFFFFFF);
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
            return api_return_axsreg(host_rand_64() & 0x7FFFFFFF);
        case 0x05:
            return api_return_axsreg(com_get_bel());
        case 0x06:
            return api_return_axsreg(pro_has_launcher());
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
            /* oem_set_code_page_run is a void: a page that will not take
             * leaves the old one in force and still answers with success. */
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
        case 0x06:
            if (value > 1)
                return api_return_errno(API_EINVAL);
            pro_set_launcher(value);
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
        return api_return_errno(API_ENOSYS);
    }
}

/* No str_init: one locale and no S() callers, so the localized chain is
 * meant to collect under --gc-sections. */
static void init(void)
{
    /* Before anything can print: the ring is what carries the boot
     * narration to a log that outlives the host's. */
    log_init();
    cpu_init();
    aud_init();
    com_init();
    std_init();
    rln_init();
    term_init();
    /* Both assets ride their own data slots. The machine runs without
     * them, so a missing one is reported rather than fatal. */
    if (!uni_init())
        printf("oem: no tables\n");
    if (!kbl_init())
        printf("kbd: no layouts\n");
    kbd_init();
    mou_init();
    pad_init();
    tab_init();
    apf_init();
    vid_init();
    tim_init();
    rand_init();
}

/* The 6502 coming out of reset. */
static void run(void)
{
    pro_run();
    com_run();
    rln_run();
    api_run();
    clk_run();
    cpu_run(); /* Must be last: this is RESB going high. */
}

/* The 6502 going into reset. Nothing here is on the platform's reset, so
 * anything a program left running is the firmware's to put back. */
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
    tab_stop();
    aud_stop();
    /* argv belongs to the image, not the run, so it is not cleared here. */
    pro_stop();
    /* Last, where the RIA's deferred vga_task puts it. */
    main_xreg_1(0x0F, 0x01, 437);
    main_xreg_1(0x0F, 0x00, vga_get_display_type());
}

/* A hot reload with bit 6 clear sends no 0x008A; the size posted into
 * MMIO_SLOT is the whole announcement. The 0x008A count is watched
 * beside it because that is what the documentation promises. */
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

/* No monitor here, so there is nothing a break could drop into. */
bool main_break(void)
{
    return false;
}

/* Alt-F4. Stopping is enough, because pro_stop puts the launcher back. */
bool main_break_to_launcher(void)
{
    if (!pro_has_launcher() || pro_is_launcher())
        return false;
    api_set_ax(0xFFFF);
    main_stop();
    return true;
}

static void main_stage(void);

/* Whether a blob is arriving right now. Set on the edge and refreshed
 * every pass, which is what the two watchers below stand down on. */
static bool main_wake_pending;

/* Whether THIS BOOT declined to stage a ROM because a blob was already
 * in the window -- a different question from the one above, and the
 * only one main_wake_failed may ask. Sharing the flag meant a refusal
 * arriving into a running machine read the blob's own arrival as a
 * boot that had staged nothing, and cold-booted the ROM over the
 * session the refusal exists to preserve.
 *
 * Never observed on hardware: a memory load does not boot the machine,
 * so nothing declines. Kept because a real wake is a core launch and
 * has not been tested. */
static bool main_boot_declined;

/* What the boot saw, for the wake log to say once the host is quiet. */
bool main_boot_wake;
uint32_t main_boot_slot;
uint8_t main_boot_upd;

/* Both readings are re-taken so the watchers below stand down. A memory
 * load re-announces nothing, so there is usually nothing to suppress; a
 * core launch announces everything, and none of it is news, because
 * what is staged is what this machine is already running. */
void main_restored(void)
{
    main_wake_pending = false;
    main_upd_seen = (uint8_t)MMIO_UPD_N;
    MMIO_SLOT = 0;
}

/* The other way out, and only for a boot that declined to stage. Such a
 * boot has nothing at all -- it never staged, and a refusal writes
 * nothing -- so the ROM the host announced is staged here after all. A
 * refusal into a running machine is a no-op, which is the whole
 * difference between the two cases. */
void main_wake_failed(void)
{
    if (!main_boot_declined)
        return;
    main_boot_declined = false;
    main_stage();
}

static void main_stage(void)
{
    uint32_t len;
    bool staged = main_rom_len(&len);
    bool ok = staged && rom_load_staged(len);
    /* After the load, not before: Get File needs the host, which at boot
     * is still staging, and asking first burns the bridge's whole deadline. */
    pro_restage();
    /* Cleared once the image is dealt with, so a watcher can tell a load
     * in progress from a finished one. */
    MMIO_SLOT = 0;
    if (ok)
        main_run();
    else if (staged)
        printf("rom: bad image\n");
}

int main(void)
{
    init();

    /* A blob already in the window means the host is waking this core
     * rather than starting it, and the restore that is coming will
     * replace everything a staged ROM would put here. Starting one
     * under it is a cold boot the user watches get rolled back. */
    main_wake_pending = sst_pending();
    /* Kept, not printed: the moment it is knowable is the moment the
     * host may be streaming a savestate in, and a console busy then
     * starves the staging store's write drain against a bridge that
     * does not wait. The bench stops on exactly that. It is said later,
     * from the wake log, when nothing is in flight.
     *
     * Measured on hardware, this reads zero on every wake -- the host
     * writes the blob only after Reset Exit -- so the check here is
     * kept for the case where a blob does precede the boot, and the
     * one that does the work is in the loop below. */
    main_boot_wake = main_wake_pending;
    main_boot_declined = main_wake_pending;
    main_boot_slot = MMIO_SLOT;
    main_boot_upd = (uint8_t)MMIO_UPD_N;
    LOG_SAY("main: boot wake=%u slot=%08x upd=%u\n",
               (unsigned)main_boot_wake, (unsigned)main_boot_slot,
               (unsigned)main_boot_upd);
    if (!main_wake_pending)
        main_stage();
    /* Whatever the host has announced up to here is this image. */
    main_upd_seen = (uint8_t)MMIO_UPD_N;

    for (;;)
    {
        /* Loop-local: both states it can leave are handled below, so the
         * news cannot outlive the pass that took it. */
        bool restage = false;

        if (API_PENDING)
        {
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
        apf_task();
        kbt_task(); /* the repeat timer; apf_task does the reports */
        std_task();
        com_task();
        log_task();
        bel_task();
        rln_task();
        term_task();
        vid_task();
        api_task();
        sst_task();
        /* Asked every pass, because at boot there was nothing to see.
         * The host writes the blob after Reset Exit, so the first
         * bridge write into the window arrives with this device's own
         * cold-booted program already running -- and everything that
         * program does from here is about to be replaced by the blob,
         * while a file it creates or truncates on the way is not. So
         * the moment a blob starts arriving the program is stopped,
         * which is the most this side can do about a boot it was never
         * given the chance to decline.
         *
         * The bit clears in fabric when the load lands, so this is a
         * question and not a latch: a program launched after a wake
         * still starts. */
        bool wake = sst_pending();
        if (wake && !main_wake_pending)
        {
            /* A blob has started arriving. Said once, on the edge: what
             * follows is either a restore or a refusal, and a log with
             * this line and neither of those says the engine never
             * finished. */
            LOG_SAY("main: blob\n");
            main_stop();
        }
        main_wake_pending = wake;

        /* Both watchers stand down while a restore is expected:
         * anything announced then is the program this machine already
         * has, and main_restored takes both readings once the blob has
         * landed. */
        uint8_t upd = (uint8_t)MMIO_UPD_N;
        if (upd != main_upd_seen && !main_wake_pending)
        {
            main_upd_seen = upd;
            restage = true;
            main_stop();
        }
        /* main_stage cleared this after the boot image, so anything
         * standing here again is a fresh settle. Left set until the
         * restage clears it, which is what tb_quiet reads. */
        if (MMIO_SLOT && !main_wake_pending)
        {
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
        /* Below both branches rather than inside stopping: a machine
         * already stopped is owed no stop and would otherwise never launch. */
        if (main_state == stopped)
        {
            if (restage)
            {
                /* No com_stop soft reset on this host, so a replaced program
                 * would otherwise run its first line into the last line of
                 * the one before. An exec gets nothing — it reads as one
                 * session. */
                com_putchar('\n');
                main_stage();
            }
            else if (pro_exec_take())
                main_run();
        }
    }
}
