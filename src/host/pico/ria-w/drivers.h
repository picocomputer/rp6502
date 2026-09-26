/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
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
#include "core/rom/rom.h"
#include "ria/mon/rom.h"
#include "ria-w/ble/ble.h"
#include "ria-w/net/cyw.h"
#include "ria-w/net/modem.h"
#include "ria-w/net/ntp.h"
#include "ria-w/net/wifi.h"
#include "ria/sys/rp2350.h"
#include "ria/sys/com.h"
#include "ria/sys/com_telnet.h"
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

/* RP2350_DRIVER is first because later inits, such as vga_init, compute clock
 * dividers from the system clock. MON_DRIVER comes before every driver whose
 * init can queue a message, because in a release build the banner that
 * mon_init queues clears the terminal and would erase a message queued ahead
 * of it. PIX_DRIVER comes before VGA_DRIVER because vga_init sends over PIX,
 * and LFS_DRIVER comes before CFG_DRIVER because cfg_init reads the config
 * file from littlefs. USB_DRIVER comes near the end because every init after
 * usb_init runs before usb_task can enumerate a device, so each one uses up
 * part of the boot enumeration window that usb_init starts. keyboard_mount
 * turns NumLock off only when a keyboard listed in
 * keyboard_numlock_off_at_boot mounts inside that window.
 *
 * sys_io_task calls the io_task column in this same order. ROM_DRIVER comes
 * before NFC_DRIVER and API_DRIVER because nfc_task and api_task can call
 * sys_stop and then start a ROM load. sys_active stays true until sys_commit
 * runs the stop hooks at the end of the pass, so if rom_task ran later in the
 * same pass, it could call ria_write_buf, which asserts that sys_active is
 * false. */
#define RP6502_MACH_DRIVERS                          \
    RP2350_DRIVER,                                   \
    COM_DRIVER, COM_TELNET_DRIVER, MON_DRIVER,       \
    RIA_DRIVER, PIX_DRIVER, VGA_DRIVER,              \
    LFS_DRIVER, CFG_DRIVER,                          \
    PROC_DRIVER, STR_DRIVER, STD_DRIVER,             \
    CYW_DRIVER, WIFI_DRIVER, NTP_DRIVER, BLE_DRIVER, \
    OEM_DRIVER, LED_DRIVER,                          \
    AUD_DRIVER, MID_DRIVER, KEYBOARD_DRIVER,         \
    KEYMAP_DRIVER, MOUSE_DRIVER, GAMEPAD_DRIVER,     \
    TABLET_DRIVER,                                   \
    MBUF_DRIVER, RLN_DRIVER, FIL_DRIVER,              \
    ROM_DRIVER, UF2_DRIVER, TIM_DRIVER,              \
    MODEM_DRIVER, DIR_DRIVER, CLK_DRIVER,            \
    DRIVE_DRIVER, RAM_DRIVER,                        \
    VCP_DRIVER, NFC_DRIVER, API_DRIVER,              \
    USB_DRIVER, PHI2_DRIVER, RESB_DRIVER

/* std_api_open tries these rows in order, and fs_std_handles accepts every
 * path, so FS_STD_DRIVER is last. */
#define RP6502_STD_DRIVERS                           \
    MODEM_STD_DRIVER, VCP_STD_DRIVER,                \
    MID_STD_DRIVER, ROM_STD_DRIVER,                  \
    NFC_STD_DRIVER, SAVE_STD_DRIVER, FS_STD_DRIVER

#define RP6502_COM_SOURCES                     \
    [COM_SOURCE_KEYBOARD] = KEYMAP_COM_SOURCE, \
    [COM_SOURCE_UART] = COM_UART_SOURCE,       \
    [COM_SOURCE_TEL] = COM_TELNET_SOURCE

#endif /* _HOST_DRIVERS_H_ */
