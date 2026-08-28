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
#include "ria/usb/nfc.h"
#include "ria/usb/vcp.h"
#include "ria/mon/uf2.h"
#include "ria/ble/ble.h"
#include "ria/net/ntp.h"
#include "ria/net/wifi.h"

/* The first eight are the machine's bring-up, and the order is the fabric's:
 * the clock before everything timed against it, the console before anything
 * prints, the banner before anything can queue an error under it, the bus
 * before the video that talks over it, and the filesystem before the config it
 * holds. The rest is init order and little else -- cyw before the three radio
 * users, api before cpu so the registers are released before RESB rises, usb
 * second-to-last because its enumeration window times a keyboard quirk and
 * anything slow scheduled inside it stops the quirk firing, cpu last.
 *
 * The io_task column reads its order off this same list, and one rule is
 * load-bearing there: rom before vcp, nfc and api, with api the last row that
 * has one. api_task and nfc_task can arm an exec, and rom_task must not run
 * after the arming in the same pass. vcp before nfc, which opens the device
 * index vcp sets. */
#define ROSTER                                                            \
    SYS_MACH_LIFECYCLE, COM_MACH_LIFECYCLE, MON_MACH_LIFECYCLE,           \
    RIA_MACH_LIFECYCLE, PIX_MACH_LIFECYCLE, VGA_MACH_LIFECYCLE,           \
    LFS_MACH_LIFECYCLE, CFG_MACH_LIFECYCLE,                               \
    PROC_MACH_LIFECYCLE, STR_MACH_LIFECYCLE, STD_MACH_LIFECYCLE,          \
    CYW_MACH_LIFECYCLE, WIFI_MACH_LIFECYCLE, NTP_MACH_LIFECYCLE,          \
    BLE_MACH_LIFECYCLE, OEM_MACH_LIFECYCLE, LED_MACH_LIFECYCLE,           \
    AUD_MACH_LIFECYCLE, MID_MACH_LIFECYCLE, KEYBOARD_MACH_LIFECYCLE,      \
    KEYMAP_MACH_LIFECYCLE, MOUSE_MACH_LIFECYCLE, GAMEPAD_MACH_LIFECYCLE,  \
    TABLET_MACH_LIFECYCLE,                                                \
    MEM_MACH_LIFECYCLE, RLN_MACH_LIFECYCLE, FIL_MACH_LIFECYCLE,           \
    ROM_MACH_LIFECYCLE, UF2_MACH_LIFECYCLE, TIM_MACH_LIFECYCLE,           \
    MODEM_MACH_LIFECYCLE, DIR_MACH_LIFECYCLE, CLK_MACH_LIFECYCLE,         \
    DRIVE_MACH_LIFECYCLE, RAM_MACH_LIFECYCLE,                             \
    VCP_MACH_LIFECYCLE, NFC_MACH_LIFECYCLE, API_MACH_LIFECYCLE,           \
    USB_MACH_LIFECYCLE, CPU_MACH_LIFECYCLE

void lifecycle_init(void)
{
#define LIFECYCLE(i, t, iot, r, s, b) i();
    LIFECYCLE_FORWARD(ROSTER)
#undef LIFECYCLE
}

void lifecycle_on_run(void)
{
#define LIFECYCLE(i, t, iot, r, s, b) r();
    LIFECYCLE_FORWARD(ROSTER)
#undef LIFECYCLE
}

void lifecycle_on_stop(void)
{
#define LIFECYCLE(i, t, iot, r, s, b) s();
    LIFECYCLE_REVERSE(ROSTER)
#undef LIFECYCLE
}

/* Backward, like stop: a break is a teardown. It is also what puts com_break
 * near the last, where the newline it writes lands after whatever the other
 * breaks printed. */
void lifecycle_break_drivers(void)
{
#define LIFECYCLE(i, t, iot, r, s, b) b();
    LIFECYCLE_REVERSE(ROSTER)
#undef LIFECYCLE
}
