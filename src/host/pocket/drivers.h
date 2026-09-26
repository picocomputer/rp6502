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
#include "core/api/proc.h"
#include "core/api/std.h"
#include "core/api/tim.h"
#include "core/com/com.h"
#include "core/hid/gamepad.h"
#include "core/hid/keyboard.h"
#include "core/hid/keymap.h"
#include "core/hid/layout.h"
#include "core/hid/mouse.h"
#include "core/hid/tablet.h"
#include "core/str/rln.h"
#include "core/str/str.h"
#include "core/str/unicode.h"
#include "core/term/term.h"
#include "core/aud/mix.h"
#include "sw/apf.h"
#include "core/wdc/resb.h"
#include "core/wdc/phi2.h"
#include "sw/proc.h"
#include "sw/wake.h"
#include "sw/bel.h"
#include "sw/cfg.h"
#include "sw/fs.h"
#include "sw/vid.h"
#include "osal/fs.h"
#include "core/rom/rom.h"

/* VID_DRIVER follows TERM_DRIVER because vid_init selects the console canvas
 * through vga_canvas_select, and vga_canvas_select then calls mode0_prog,
 * which calls term_set_height on the terminal state that term_init sets up.
 *
 * FS_DRIVER follows STD_DRIVER because stop hooks run in reverse, and fs_stop
 * has to collect a command that a stopped program left in flight before
 * std_stop's closes send their Flush.
 */
#define RP6502_MACH_DRIVERS                             \
    CFG_DRIVER, PROC_DRIVER,                            \
    STR_DRIVER,                                         \
    AUD_DRIVER, BEL_DRIVER,                             \
    COM_DRIVER, STD_DRIVER, FS_DRIVER,                  \
    RLN_DRIVER, TERM_DRIVER,                            \
    UNICODE_DRIVER, LAYOUT_DRIVER, KEYBOARD_DRIVER,     \
    APF_DRIVER, KEYMAP_DRIVER,                          \
    MOUSE_DRIVER, GAMEPAD_DRIVER, TABLET_DRIVER,        \
    VID_DRIVER, TIM_DRIVER,                             \
    DIR_DRIVER, API_DRIVER, WAKE_DRIVER,                \
    CLK_DRIVER, PHI2_DRIVER

/* open() tries these rows in order, and the filesystem row accepts every
 * path, so it is last. */
#define RP6502_STD_DRIVERS ROM_STD_DRIVER, SAVE_STD_DRIVER, FS_STD_DRIVER

/* No serial line feeds the UART row on this machine, but the row stays
 * because the terminal's replies to a program's queries arrive through it. */
#define RP6502_COM_SOURCES                     \
    [COM_SOURCE_KEYBOARD] = KEYMAP_COM_SOURCE, \
    [COM_SOURCE_UART] = COM_UART_SOURCE

#endif /* _HOST_DRIVERS_H_ */
