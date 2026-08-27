/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This machine's roster: the drivers it is made of, in the order it comes up.
 * core/lifecycle.c walks it -- forward for init, run and break, backward for
 * stop.
 *
 * What is not in it is this machine's electrical bring-up, which has to happen
 * before anything shared runs and in an order the fabric dictates rather than
 * the roster.
 */

#include "core/lifecycle.h"

#include "core/api/api.h"
#include "core/api/clk.h"
#include "core/api/dir.h"
#include "core/api/proc.h"
#include "core/api/std.h"
#include "core/api/tim.h"
#include "core/hid/gamepad.h"
#include "core/hid/keyboard.h"
#include "core/hid/keymap.h"
#include "core/hid/mouse.h"
#include "core/hid/tablet.h"
#include "core/str/oem.h"
#include "core/str/rln.h"
#include "core/str/str.h"
#include "ria/mon/drive.h"
#include "ria/mon/fil.h"
#include "ria/mon/mon.h"
#include "ria/mon/ram.h"
#include "ria/mon/rom.h"
#include "ria/net/cyw.h"
#include "ria/net/modem.h"
#include "ria/sys/com.h"
#include "ria/sys/cpu.h"
#include "ria/sys/led.h"
#include "ria/sys/mem.h"
#include "ria/sys/pix.h"
#include "ria/sys/ria.h"
#include "ria/sys/sys.h"
#include "ria/sys/vga.h"
#include "core/aud/aud.h"
#include "ria/sys/lfs.h"
#include "ria/sys/cfg.h"
#include "ria/usb/usb.h"
#include "ria/usb/mid.h"

/* ria, com and mon lead so that reversal puts their stops at the tail, where
 * the stop order needs them: ria_stop last because the three stops above it
 * read ria_active(), com_stop next-to-last because it writes the newline. */
#define ROSTER                                                      \
    RIA_HW_LIFECYCLE, COM_HW_LIFECYCLE, MON_LIFECYCLE,              \
    PROC_LIFECYCLE, STR_LIFECYCLE, STD_LIFECYCLE,                   \
    CYW_LIFECYCLE, OEM_LIFECYCLE, LED_LIFECYCLE,                    \
    AUD_LIFECYCLE, MID_LIFECYCLE, KEYBOARD_LIFECYCLE,               \
    KEYMAP_LIFECYCLE, MOUSE_LIFECYCLE, GAMEPAD_LIFECYCLE,           \
    TABLET_LIFECYCLE, ROM_LIFECYCLE, TIM_LIFECYCLE,                 \
    MODEM_LIFECYCLE, RLN_LIFECYCLE, DIR_LIFECYCLE,                  \
    PIX_LIFECYCLE, VGA_HW_LIFECYCLE, API_LIFECYCLE,                 \
    CLK_LIFECYCLE, MEM_HW_LIFECYCLE, DRIVE_LIFECYCLE,               \
    FIL_LIFECYCLE, RAM_LIFECYCLE, USB_LIFECYCLE, CPU_LIFECYCLE

void lifecycle_init(void)
{
    /* Order here is the fabric's, not the roster's. */
    com_init();  /* stdio dispatcher first: DBG() prints */
    sys_init();  /* queues the startup message, before anything can queue an error */
    ria_init();  /* bus pins, four PIO programs, core1 */
    pix_init();
    vga_init();  /* after pix: it disables the backchannel through it */
    lfs_init();
    cfg_init();  /* on lfs, and before every row below that adopts a default */
#define LIFECYCLE(i, r, s, b) i();
    LIFECYCLE_FORWARD(ROSTER)
#undef LIFECYCLE
}

void lifecycle_on_run(void)
{
#define LIFECYCLE(i, r, s, b) r();
    LIFECYCLE_FORWARD(ROSTER)
#undef LIFECYCLE
}

void lifecycle_on_stop(void)
{
#define LIFECYCLE(i, r, s, b) s();
    LIFECYCLE_REVERSE(ROSTER)
#undef LIFECYCLE
}

void lifecycle_break_drivers(void)
{
#define LIFECYCLE(i, r, s, b) b();
    LIFECYCLE_FORWARD(ROSTER)
#undef LIFECYCLE
    com_break(); /* the newline goes after whatever the rest of them printed */
}
