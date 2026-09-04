/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This machine's drivers: the ones it is made of and the order it comes up
 * in, and the ones it offers a program to open. Both are the same kind of
 * fact, so they are the same file.
 *
 * src/host/pico/ria/main.c walks the machine rows -- forward to bring up and
 * to pump, backward to tear down. core/api/std.c builds the table from the
 * stdio rows. The drive a path reaches is in neither list: osal/dir.h
 * names those calls and the host that is linked defines them.
 */

#ifndef _HOST_DRIVERS_H_
#define _HOST_DRIVERS_H_

#include "core/sys/driver.h"

#include "core/api/api.h"
#include "core/api/clk.h"
#include "core/api/dir.h"
#include "osal/fs.h"
#include "ria/api/proc.h"
#include "core/api/std.h"
#include "ria/api/tim.h"
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
#include "core/rom/rom.h" /* ROM_STD_DRIVER: the one asset driver */
#include "ria/mon/rom.h"
#include "ria/sys/rp2350.h"
#include "ria/sys/com.h"
#include "ria/sys/phi2.h"
#include "ria/sys/resb.h"
#include "ria/sys/led.h"
#include "ria/sys/mbuf.h"
#include "ria/sys/pix.h"
#include "ria/sys/ria.h"
#include "ria/sys/vga.h"
#include "core/aud/mix.h"
#include "osal/pico/lfs.h"
#include "ria/sys/cfg.h"
#include "ria/usb/usb.h"
#include "ria/usb/mid.h"
#include "ria/usb/nfc.h"
#include "ria/usb/vcp.h"
#include "ria/mon/uf2.h"

/* The first eight are the machine's bring-up, and the order is the fabric's:
 * the part's clock before anything divided from it is set up, the console
 * before anything prints, the banner before anything can queue an
 * error under it, the bus before the video that talks over it, and the
 * filesystem before the config it holds. The rest is init order and little
 * else -- usb second-to-last because its enumeration window times a keyboard
 * quirk and anything slow scheduled inside it stops the quirk firing, and phi2
 * after it because a reclock is exactly that kind of slow. phi2 must also come
 * after ria and pix, whose inits create the state machines it reprograms.
 *
 * The io_task column reads its order off this same list, and one rule is
 * load-bearing there: rom before vcp, nfc and api, with api the last row that
 * has one. api_task and nfc_task can arm an exec, and rom_task must not run
 * after the arming in the same pass. vcp before nfc, which opens the device
 * index vcp sets. */
#define RP6502_MACH_DRIVERS                          \
    RP2350_DRIVER,                                   \
    COM_DRIVER, MON_DRIVER,                          \
    RIA_DRIVER, PIX_DRIVER, VGA_DRIVER,              \
    LFS_DRIVER, CFG_DRIVER,                          \
    PROC_DRIVER, STR_DRIVER, STD_DRIVER,             \
    OEM_DRIVER, LED_DRIVER,                          \
    AUD_DRIVER, MID_DRIVER, KEYBOARD_DRIVER,         \
    KEYMAP_DRIVER, MOUSE_DRIVER, GAMEPAD_DRIVER,     \
    TABLET_DRIVER,                                   \
    MBUF_DRIVER, RLN_DRIVER, FIL_DRIVER,              \
    ROM_DRIVER, UF2_DRIVER, TIM_DRIVER,              \
    DIR_DRIVER, CLK_DRIVER,                          \
    DRIVE_DRIVER, RAM_DRIVER,                        \
    VCP_DRIVER, NFC_DRIVER, API_DRIVER,              \
    USB_DRIVER, PHI2_DRIVER, RESB_DRIVER

/* What a program may open, in the order open() tries them. The filesystem is
 * the catch-all, so it is last. */
#define RP6502_STD_DRIVERS                           \
    VCP_STD_DRIVER, MID_STD_DRIVER,                  \
    ROM_STD_DRIVER, NFC_STD_DRIVER, FS_STD_DRIVER

#endif /* _HOST_DRIVERS_H_ */
