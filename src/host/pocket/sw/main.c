/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/api/xreg.h"
#include "drivers.h"

#include "apf.h"
#include "cfg.h"
#include "font.h"
#include "main.h"
#include "mmio.h"
#include "fs.h"
#include "proc.h"
#include "rom.h"
#include "core/rom/rom.h"
#include "vga.h"
#include "vid.h"
#include "wake.h"
#include "core/api/api.h"
#include "core/api/attr.h"
#include "core/api/clk.h"
#include "core/api/proc.h"
#include "osal/dir.h"
#include "core/api/std.h"
#include "core/api/tim.h"
#include "core/com/com.h"
#include "core/str/unicode.h"
#include "core/hid/keyboard.h"
#include "core/hid/keymap.h"
#include "core/hid/layout.h"
#include "core/hid/mouse.h"
#include "core/hid/gamepad.h"
#include "core/hid/tablet.h"
#include "core/sys/sys.h"
#include "core/sys/debug_log.h"
#include "core/com/rln.h"
#include "core/sys/pix.h"
#include "core/term/term.h"


#include <stdint.h>
#include <stdio.h>

/* The C library's stdout and stderr are the console. */
static int main_stdio_putc(char c, FILE *f)
{
    (void)f;
    return com_putchar((unsigned char)c);
}

static FILE main_stdio = FDEV_SETUP_STREAM(main_stdio_putc, NULL, NULL,
                                           _FDEV_SETUP_WRITE);
FILE *const stdout = &main_stdio;
FILE *const stderr = &main_stdio;

static uint8_t main_upd_seen;

static void main_stage(void);

static bool main_wake_pending;

static bool main_boot_declined;

bool main_boot_wake;
uint32_t main_boot_slot;
uint8_t main_boot_upd;

void main_restored(void)
{
    main_wake_pending = false;
    main_upd_seen = (uint8_t)MMIO_UPD_N;
    MMIO_SLOT = 0;
}

void main_wake_failed(void)
{
    if (!main_boot_declined)
        return;
    main_boot_declined = false;
    main_stage();
}

static void main_stage(void)
{
    rom_assets_reset();
    api_errno err;
    int fd = fs_rom_adopt(&err);
    bool staged = fd >= 0;
    bool ok = staged && rom_load_fd(fd);
    proc_restage();
    /* MMIO_SLOT is cleared only after the load has succeeded or failed,
     * because tb_quiet in tests/bench/tb_quiet.h does not count a run with
     * an image as quiet until the register behind MMIO_SLOT reads zero. */
    MMIO_SLOT = 0;
    if (ok)
        sys_run();
    else if (staged)
        RP6502_LOG(rom, ERROR, "bad image");
}

int main(void)
{
    sys_init();

    main_wake_pending = wake_pending();
    main_boot_wake = main_wake_pending;
    main_boot_declined = main_wake_pending;
    main_boot_slot = MMIO_SLOT;
    main_boot_upd = (uint8_t)MMIO_UPD_N;
    if (!main_wake_pending)
        main_stage();
    main_upd_seen = (uint8_t)MMIO_UPD_N;

    for (;;)
    {
        bool restage = false;

        /* DROP is done in the fabric. EXIT holds the 6502 until
         * proc_exit has it in reset. */
        if (API_PENDING)
        {
            API_PENDING = 0;
            if (API_OP == 0xFF)
            {
                proc_exit((int16_t)API_AX);
                api_return_ax(0);
            }
        }
        sys_task();
        /* The program is stopped as soon as a blob starts to arrive,
         * because the restore replaces the machine's memory but not a
         * file the program creates or truncates before the load. */
        bool wake = wake_pending();
        if (wake && !main_wake_pending)
            sys_stop();
        main_wake_pending = wake;

        uint8_t upd = (uint8_t)MMIO_UPD_N;
        if (upd != main_upd_seen && !main_wake_pending)
        {
            main_upd_seen = upd;
            restage = true;
            proc_cancel_launcher();
            sys_stop();
        }
        if (MMIO_SLOT && !main_wake_pending)
        {
            restage = true;
            proc_cancel_launcher();
            sys_stop();
        }
        sys_commit();
        /* The restage starts after sys_commit rather than from a stop hook,
         * because the stop hooks run only when a running machine is
         * stopped, so a restage requested while the machine is already
         * stopped would never start. Nothing starts while a blob arrives, for
         * the reason the program was stopped, so an exec or a launcher's
         * return waits for the load to fail or to replace it. */
        if (!sys_active() && !main_wake_pending)
        {
            if (restage)
                main_stage();
            else if (proc_exec_take())
                sys_run();
        }
    }
}
