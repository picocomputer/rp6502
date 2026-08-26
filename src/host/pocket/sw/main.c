/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The soft CPU's runner, this platform's host/pico/ria/main.c.
 */

#include <stdio.h>

#include "apf.h"
#include "aud.h"
#include "bel.h"
#include "cfg.h"
#include "com.h"
#include "cpu.h"
#include "font.h"
#include "log.h"
#include "main.h"
#include "mmio.h"
#include "msc.h"
#include "proc.h"
#include "rand.h"
#include "rom.h"
#include "sst.h"
#include "vga.h"
#include "vid.h"
#include "core/api/api.h"
#include "core/api/attr.h"
#include "core/api/clk.h"
#include "core/api/proc.h"
#include "core/api/dir.h"
#include "core/api/std.h"
#include "core/api/tim.h"
#include "core/api/uni.h"
#include "core/hid/keyboard.h"
#include "core/hid/keymap.h"
#include "core/hid/layout.h"
#include "core/hid/mouse.h"
#include "core/hid/gamepad.h"
#include "core/hid/tablet.h"
#include "core/main.h"
#include "core/str/rln.h"
#include "core/pix.h"
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

/* No fabric path raises the 6502's IRQ for this, so a signal here is what a
 * program finds when it asks rather than something that interrupts it. The
 * latch still has to exist, or Ctrl-C is a keystroke that does nothing. */
static bool ria_sigint;

void ria_trigger_sigint(void)
{
    ria_sigint = true;
}

bool ria_get_sigint(void)
{
    bool latched = ria_sigint;
    ria_sigint = false;
    return latched;
}

/* std.c wants the catch-all last, so the mass-storage drive follows ROM. */
const dir_backend_t *main_dir_backend(void)
{
    return &msc_dir_backend;
}

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
    if (!layout_init())
        printf("keyboard: no layouts\n");
    keyboard_init();
    keymap_init(); /* the speller is this machine's, not the device layer's */
    mouse_init();
    gamepad_init();
    tablet_init();
    apf_init();
    vid_init();
    tim_init();
    rand_init();
}

/* The 6502 coming out of reset. */
void main_on_run(void)
{
    proc_run();
    com_run();
    rln_run();
    api_run();
    clk_run();
    cpu_run(); /* Must be last: this is RESB going high. */
}

/* The 6502 going into reset. Nothing here is on the platform's reset, so
 * anything a program left running is the firmware's to put back. */
void main_on_stop(void)
{
    cpu_stop(); /* Must be first. */
    rln_stop();
    api_stop();
    std_stop();
    msc_stop(); /* after std_stop: its closes are what park a read */
    keyboard_stop();
    mouse_stop();
    gamepad_stop();
    tablet_stop();
    aud_stop();
    /* argv belongs to the image, not the run, so it is not cleared here. */
    proc_stop();
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

/* No monitor here, so there is nothing a break could drop into. */
bool main_break(void)
{
    return false;
}

/* Alt-F4. Stopping is enough, because proc_stop puts the launcher back. */
bool main_break_to_launcher(void)
{
    if (!proc_has_launcher() || proc_is_launcher())
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
    proc_restage();
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
                /* Captured before api_return_ax clobbers A/X. */
                proc_set_exit_code((int16_t)API_AX);
                main_stop();
                api_return_ax(0);
            }
        }
        cfg_task();
        apf_task();
        keymap_task(); /* the repeat timer; apf_task does the reports */
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
        main_commit();
        /* Below the commit rather than inside it: a machine already stopped is
         * owed no stop and would otherwise never launch. */
        if (!main_active())
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
            else if (proc_exec_take())
                main_run();
        }
    }
}
