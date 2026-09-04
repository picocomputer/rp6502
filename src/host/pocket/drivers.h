/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This machine's drivers: the ones it is made of and the order it comes up
 * in, and the ones it offers a program to open. Both are the same kind of
 * fact, so they are the same file.
 *
 * src/host/pocket/sw/main.c walks the machine rows -- forward to bring up
 * and to pump, backward to tear down. core/api/std.c builds the table from
 * the stdio rows.
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
#include "sw/sst.h"
#include "sw/bel.h"
#include "sw/cfg.h"
#include "sw/fs.h"
#include "sw/vid.h"
#include "osal/fs.h"
#include "core/rom/rom.h"

/* aud before com, so the bell hardware is quiet before the byte path that
 * can ring it is armed. fs before std, so reversal puts fs_stop after
 * std_stop -- std's closes are what park a read. unicode and layout before
 * keymap, which asks them what layouts exist. vid after term, whose height
 * its canvas sets.
 *
 * apf before keymap is the task column's rule, not init's: apf_task delivers
 * the reports and keymap_task runs the repeat timer over them. bel takes no
 * init -- the bell is part of the mixer aud brings up and restores.
 */
#define RP6502_MACH_DRIVERS                             \
    CFG_DRIVER, PROC_DRIVER,                            \
    STR_DRIVER,                                         \
    AUD_DRIVER, BEL_DRIVER,                             \
    COM_DRIVER, FS_DRIVER,                              \
    STD_DRIVER, RLN_DRIVER, TERM_DRIVER,                \
    UNICODE_DRIVER, LAYOUT_DRIVER, KEYBOARD_DRIVER,     \
    APF_DRIVER, KEYMAP_DRIVER,                          \
    MOUSE_DRIVER, GAMEPAD_DRIVER, TABLET_DRIVER,        \
    VID_DRIVER, TIM_DRIVER,                             \
    DIR_DRIVER, API_DRIVER, SST_DRIVER,                 \
    CLK_DRIVER, PHI2_DRIVER

/* What a program may open, in the order open() tries them. The filesystem is
 * the catch-all, so it is last. */
#define RP6502_STD_DRIVERS ROM_STD_DRIVER, FS_STD_DRIVER

#endif /* _HOST_DRIVERS_H_ */
