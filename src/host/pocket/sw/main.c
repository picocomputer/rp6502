/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/api/xreg.h"
#include "drivers.h"

#include "apf.h"
#include "cfg.h"
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
#include "core/com/manifold.h"
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
#include "core/term/cells.h"


#include <stdint.h>
#include <stdio.h>

/* The C library's stdout and stderr are the console. */
static int main_stdio_putc(char c, FILE *f)
{
    (void)f;
    return manifold_putchar((unsigned char)c);
}

static FILE main_stdio = FDEV_SETUP_STREAM(main_stdio_putc, NULL, NULL,
                                           _FDEV_SETUP_WRITE);
FILE *const stdout = &main_stdio;
FILE *const stderr = &main_stdio;

static uint8_t main_upd_seen;

/* A start of the staged ROM asked for and not yet made. The boot asks for
 * the first one. */
static bool main_restaging;

void main_restored(void)
{
    main_upd_seen = (uint8_t)MMIO_UPD_N;
    MMIO_SLOT = 0;
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

    main_upd_seen = (uint8_t)MMIO_UPD_N;
    main_restaging = true;

    for (;;)
    {
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

        /* The host asks for a start by loading a ROM, which changes
         * MMIO_UPD_N and MMIO_SLOT. A wake's host sets both as well, and
         * wake_task marks them handled before it ends a restore, so neither
         * is acted on while a wake is in progress. They are read before the
         * wake is, because a save can halt the firmware anywhere: what was
         * read before the save is then from the session that was saved, and
         * a restore that came between the reads shows as a wake. */
        uint8_t upd = (uint8_t)MMIO_UPD_N;
        bool slot = MMIO_SLOT != 0;
        /* A new ROM is being written over the old one's file. The bitstream
         * already holds the 6502; the program stops here and nothing
         * starts until the image has landed and MMIO_SLOT says so. */
        bool coming = ROM_COMING != 0;
        bool wake = wake_pending();
        if (!wake && (slot || upd != main_upd_seen))
        {
            main_upd_seen = upd;
            main_restaging = true;
        }
        if ((main_restaging || coming) && !wake)
        {
            proc_cancel_launcher();
            sys_stop();
        }
        sys_commit();
        /* The start comes after sys_commit rather than from a stop hook,
         * because the stop hooks run only when a running machine is
         * stopped, so a start asked for while the machine is already
         * stopped would never be made. Nothing starts while a wake is in
         * progress, because the restore replaces the machine's memory but
         * not a file the program creates or truncates before the load, so
         * a start, an exec or a launcher's return waits for the wake to
         * end. */
        if (!sys_active() && !wake && !coming)
        {
            if (main_restaging)
            {
                main_restaging = false;
                main_stage();
            }
            else if (proc_exec_take())
                sys_run();
        }
    }
}
