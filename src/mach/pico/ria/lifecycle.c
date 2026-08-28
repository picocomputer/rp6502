/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This machine's roster: the drivers it is made of, in the order it comes up.
 * Forward to bring up, backward to tear down -- stop and break both, because
 * a break is a teardown and wants the same order for the same reason.
 *
 * The whole list is here. main() calls lifecycle_init and then loops.
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

/* The first eight are the machine's bring-up, and the order is the fabric's:
 * the clock before everything timed against it, the console before anything
 * prints, the banner before anything can queue an error under it, the bus
 * before the video that talks over it, and the filesystem before the config it
 * holds. The rest is init order and little else -- cyw after cfg because the
 * country code is an argument to the radio, usb late because its enumeration
 * window times a keyboard quirk, cpu last. */
#define ROSTER                                                      \
    SYS_LIFECYCLE, COM_LIFECYCLE, MON_LIFECYCLE, RIA_LIFECYCLE,     \
    PIX_LIFECYCLE, VGA_LIFECYCLE, LFS_LIFECYCLE, CFG_LIFECYCLE,     \
    PROC_LIFECYCLE, STR_LIFECYCLE, STD_LIFECYCLE,                   \
    CYW_LIFECYCLE, OEM_LIFECYCLE, LED_LIFECYCLE,                    \
    AUD_LIFECYCLE, MID_LIFECYCLE, KEYBOARD_LIFECYCLE,               \
    KEYMAP_LIFECYCLE, MOUSE_LIFECYCLE, GAMEPAD_LIFECYCLE,           \
    TABLET_LIFECYCLE, ROM_LIFECYCLE, TIM_LIFECYCLE,                 \
    MODEM_LIFECYCLE, RLN_LIFECYCLE, DIR_LIFECYCLE,                  \
    API_LIFECYCLE, CLK_LIFECYCLE, MEM_LIFECYCLE, DRIVE_LIFECYCLE,   \
    FIL_LIFECYCLE, RAM_LIFECYCLE, USB_LIFECYCLE, CPU_LIFECYCLE

void lifecycle_init(void)
{
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

/* Backward, like stop: a break is a teardown. It is also what puts com_break
 * near the last, where the newline it writes lands after whatever the other
 * breaks printed. */
void lifecycle_break_drivers(void)
{
#define LIFECYCLE(i, r, s, b) b();
    LIFECYCLE_REVERSE(ROSTER)
#undef LIFECYCLE
}
