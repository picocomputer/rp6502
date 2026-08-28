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

#ifndef _MACH_DRIVERS_H_
#define _MACH_DRIVERS_H_

#include "core/lifecycle.h"

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
#include "core/str/unicode.h"
#include "core/term/term.h"
#include "core/aud/aud.h"
#include "sw/apf.h"
#include "sw/cpu.h"
#include "sw/sst.h"
#include "sw/bel.h"
#include "sw/cfg.h"
#include "sw/log.h"
#include "sw/fs.h"
#include "sw/rand.h"
#include "sw/vid.h"
#include "core/api/fs.h"
#include "sw/rom.h"

/* aud before com, so the bell hardware is quiet before the byte path that
 * can ring it is armed. fs before std, so reversal puts fs_stop after
 * std_stop -- std's closes are what park a read. unicode and layout before
 * keymap, which asks them what layouts exist. vid after term, whose height
 * its canvas sets. cpu last, because its run is RESB going back up.
 *
 * apf before keymap is the task column's rule, not init's: apf_task delivers
 * the reports and keymap_task runs the repeat timer over them. bel takes no
 * init -- the bell is part of the mixer aud brings up and restores. */
#define RP6502_MACH_DRIVERS                                                            \
    LOG_MACH_LIFECYCLE, CFG_MACH_LIFECYCLE, PROC_MACH_LIFECYCLE,          \
    AUD_MACH_LIFECYCLE, BEL_MACH_LIFECYCLE,                               \
    COM_MACH_LIFECYCLE, FS_MACH_LIFECYCLE,                                \
    STD_MACH_LIFECYCLE, RLN_MACH_LIFECYCLE, TERM_MACH_LIFECYCLE,          \
    UNICODE_MACH_LIFECYCLE, LAYOUT_MACH_LIFECYCLE, KEYBOARD_MACH_LIFECYCLE, \
    APF_MACH_LIFECYCLE, KEYMAP_MACH_LIFECYCLE,                            \
    MOUSE_MACH_LIFECYCLE, GAMEPAD_MACH_LIFECYCLE, TABLET_MACH_LIFECYCLE,  \
    VID_MACH_LIFECYCLE, TIM_MACH_LIFECYCLE, RAND_MACH_LIFECYCLE,          \
    DIR_MACH_LIFECYCLE, API_MACH_LIFECYCLE, SST_MACH_LIFECYCLE,           \
    CLK_MACH_LIFECYCLE, CPU_MACH_LIFECYCLE

/* What a program may open, in the order open() tries them. The filesystem is
 * the catch-all, so it is last. */
#define RP6502_STD_DRIVERS ROM_STD_LIFECYCLE, FS_STD_LIFECYCLE

#endif /* _MACH_DRIVERS_H_ */
