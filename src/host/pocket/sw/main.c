/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The soft CPU's runner, this platform's host/pico/ria/main.c.
 */

#include "core/api/xreg.h"
#include "drivers.h"
#include <stdio.h>

#include "apf.h"
#include "aud.h"
#include "bel.h"
#include "cfg.h"
#include "com.h"
#include "font.h"
#include "main.h"
#include "mmio.h"
#include "fs.h"
#include "proc.h"
#include "rom.h"
#include "core/rom/rom.h"
#include "sst.h"
#include "vga.h"
#include "vid.h"
#include "core/api/api.h"
#include "core/api/attr.h"
#include "core/api/clk.h"
#include "core/api/proc.h"
#include "osal/dir.h"
#include "core/api/std.h"
#include "core/api/tim.h"
#include "core/str/unicode.h"
#include "core/hid/keyboard.h"
#include "core/hid/keymap.h"
#include "core/hid/layout.h"
#include "core/hid/mouse.h"
#include "core/hid/gamepad.h"
#include "core/hid/tablet.h"
#include "core/sys/sys.h"
#include "core/str/rln.h"
#include "core/sys/pix.h"
#include "core/vga/mode/mode1.h"
#include "core/vga/mode/mode2.h"
#include "core/vga/mode/mode3.h"
#include "core/vga/mode/mode4.h"
#include "core/vga/mode/mode5.h"
#include "core/term/term.h"


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
/* No str_init: one locale and no S() callers, so the localized chain is
 * meant to collect under --gc-sections. */
static uint8_t main_upd_seen;

/* No monitor here, so there is nothing a break could drop into. */
bool sys_break(void)
{
    return false;
}

/* Alt-F4. Stopping is enough, because proc_stop puts the launcher back. */
bool sys_break_to_launcher(void)
{
    if (!proc_has_launcher() || proc_is_launcher())
        return false;
    api_set_ax(0xFFFF);
    sys_stop();
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
    rom_assets_reset(); /* a fresh image replaces whatever was adopted */
    api_errno err;
    int fd = fs_rom_adopt(&err); /* slot 0: bound and written by the host */
    bool staged = fd >= 0;
    bool ok = staged && rom_load_fd(fd);
    /* After the load, not before: Get File needs the host, which at boot
     * is still staging, and asking first burns the bridge's whole deadline. */
    proc_restage();
    /* Cleared once the image is dealt with, so a watcher can tell a load
     * in progress from a finished one. */
    MMIO_SLOT = 0;
    if (ok)
        sys_run();
    else if (staged)
        printf("rom: bad image\n");
}

int main(void)
{
    sys_init();

    /* A blob already in the window means the host is waking this core
     * rather than starting it, and the restore that is coming will
     * replace everything a staged ROM would put here. Starting one
     * under it is a cold boot the user watches get rolled back. */
    main_wake_pending = sst_pending();
    /* Measured on hardware, this reads zero on every wake -- the host
     * writes the blob only after Reset Exit -- so the check here is
     * kept for the case where a blob does precede the boot, and the
     * one that does the work is in the loop below. */
    main_boot_wake = main_wake_pending;
    main_boot_declined = main_wake_pending;
    main_boot_slot = MMIO_SLOT;
    main_boot_upd = (uint8_t)MMIO_UPD_N;
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
                proc_exit((int16_t)API_AX);
                api_return_ax(0);
            }
        }
        /* Both columns back to back. File IO never blocks under a task pump
         * here, so the RIA's split is not this machine's -- but the drivers it
         * shares carry their column, and walking both is what reaches them. */
        sys_task();
        sys_io_task();
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
            sys_stop();
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
            sys_stop();
        }
        /* main_stage cleared this after the boot image, so anything
         * standing here again is a fresh settle. Left set until the
         * restage clears it, which is what tb_quiet reads. */
        if (MMIO_SLOT && !main_wake_pending)
        {
            restage = true;
            sys_stop();
        }
        sys_commit();
        /* Below the commit rather than inside it: a machine already stopped is
         * owed no stop and would otherwise never launch. */
        if (!sys_active())
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
                sys_run();
        }
    }
}
