/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_DRIVERS_H_
#define _HOST_DRIVERS_H_

#include "core/sys/driver.h"

#include "core/api/api.h"
#include "core/tim/clk.h"
#include "core/api/dir.h"
#include "osal/fs.h"
#include "ria/api/proc.h"
#include "core/api/std.h"
#include "core/com/console.h"
#include "core/com/tty.h"
#include "core/com/com.h"
#include "core/com/tio.h"
#include "ria/api/tim.h"
#include "core/hid/gamepad.h"
#include "core/hid/keyboard.h"
#include "core/hid/keymap.h"
#include "core/hid/layout.h"
#include "core/hid/mouse.h"
#include "core/hid/tablet.h"
#include "core/str/oem.h"
#include "core/com/rln.h"
#include "core/str/str.h"
#include "ria/mon/drive.h"
#include "ria/mon/fil.h"
#include "ria/mon/mon.h"
#include "ria/mon/ram.h"
#include "core/rom/rom.h"
#include "core/aud/pcm.h"
#include "ria/mon/rom.h"
#include "ria/sys/rp2350.h"
#include "ria/com/manifold.h"
#include "ria/sys/phi2.h"
#include "ria/sys/resb.h"
#include "ria/sys/led.h"
#include "ria/sys/mbuf.h"
#include "ria/sys/pix.h"
#include "ria/sys/ria.h"
#include "ria/sys/vga.h"
#include "ria/term/cells.h"
#include "core/aud/mix.h"
#include "osal/pico/lfs.h"
#include "ria/sys/cfg.h"
#include "ria/usb/usb.h"
#include "ria/usb/mid.h"
#include "ria/usb/nfc.h"
#include "ria/usb/vcp.h"
#include "ria/mon/uf2.h"

/* RP2350 comes first so the system clock is set before any init computes a
 * divider from it, and MANIFOLD_UART comes next so the console is up before
 * anything prints. COM follows it because com's init clears every source, the
 * UART that MANIFOLD_UART starts included. MON follows so the boot banner that
 * mon_init queues comes before any error a later init queues. PIX comes
 * before VGA because vga_init sends a PIX message, and LFS comes before CFG
 * because cfg_init reads its file from the volume that lfs_init mounts. USB
 * comes near the end because usb_init starts the boot enumeration window that
 * keymap.c checks, and every init after usb_init runs before usb_task
 * can mount a keyboard, so a slow init can use up the window first. PHI2
 * comes after RIA and PIX because phi2_init sets the clock dividers of state
 * machines that ria_init and pix_init configure.
 *
 * The tasks run in the same order. ROM must come before NFC and API, because nfc_task and api_task can start an exec that stops the
 * running program, and rom_task must not start the load until sys_commit has
 * performed that stop. */
#define RP6502_MACH_DRIVERS                          \
    RP2350_DRIVER,                                   \
    MANIFOLD_UART_DRIVER, COM_DRIVER, MON_DRIVER,    \
    RIA_DRIVER, PIX_DRIVER, CELLS_RIA_DRIVER,        \
    VGA_DRIVER,                                      \
    LFS_DRIVER, CFG_DRIVER,                          \
    PROC_DRIVER, STR_DRIVER,                         \
    STD_DRIVER, CONSOLE_DRIVER, TTY_DRIVER,          \
    OEM_DRIVER, LED_DRIVER,                          \
    AUD_DRIVER, MID_DRIVER, KEYBOARD_DRIVER,         \
    LAYOUT_DRIVER, KEYMAP_DRIVER,                    \
    MOUSE_DRIVER, GAMEPAD_DRIVER,                    \
    TABLET_DRIVER,                                   \
    MBUF_DRIVER, TIO_DRIVER, RLN_DRIVER, FIL_DRIVER,  \
    ROM_DRIVER, UF2_DRIVER, TIM_DRIVER,              \
    DIR_DRIVER, CLK_DRIVER,                          \
    DRIVE_DRIVER, RAM_DRIVER,                        \
    VCP_DRIVER, NFC_DRIVER, API_DRIVER,              \
    USB_DRIVER, PHI2_DRIVER, RESB_DRIVER

/* open() tries these drivers' STD rows in order, and the filesystem's
 * accepts every path, so it is last. ASSET_DRIVER is here for ROM:'s row
 * alone: the monitor resets the assets itself, so its hooks are not in the
 * list above. */
#define RP6502_STD_DRIVERS                           \
    CONSOLE_DRIVER, TTY_DRIVER,                      \
    VCP_DRIVER, MID_DRIVER,                          \
    ASSET_DRIVER, NFC_DRIVER,                        \
    PCM_DRIVER,                                      \
    SAVE_DRIVER, FS_DRIVER

#define RP6502_COM_SOURCES                     \
    [COM_SOURCE_KEYBOARD] = KEYMAP_COM_SOURCE, \
    [COM_SOURCE_UART] = MANIFOLD_UART_SOURCE

#endif /* _HOST_DRIVERS_H_ */
