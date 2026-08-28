/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This machine's roster: the drivers it is made of, in the order it comes up.
 * core/lifecycle.c walks it -- forward to bring up, backward to tear down.
 */

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
#include "sw/log.h"
#include "sw/fs.h"
#include "sw/rand.h"
#include "sw/vid.h"

/* aud before com, so the
 * bell hardware is quiet before the byte path that can ring it is armed. fs
 * before std, so reversal puts fs_stop after std_stop -- std's closes are
 * what park a read. unicode and layout before keymap, which asks them what
 * layouts exist. vid after term, whose height its canvas sets. cpu last,
 * because its run is RESB going back up. */
#define ROSTER                                                  \
    LOG_LIFECYCLE, PROC_LIFECYCLE,                              \
    AUD_LIFECYCLE, COM_LIFECYCLE, FS_LIFECYCLE,                 \
    STD_LIFECYCLE, RLN_LIFECYCLE, TERM_LIFECYCLE,               \
    UNICODE_LIFECYCLE, LAYOUT_LIFECYCLE, KEYBOARD_LIFECYCLE,    \
    KEYMAP_LIFECYCLE, MOUSE_LIFECYCLE, GAMEPAD_LIFECYCLE,       \
    TABLET_LIFECYCLE, VID_LIFECYCLE, APF_LIFECYCLE,             \
    TIM_LIFECYCLE, RAND_LIFECYCLE, DIR_LIFECYCLE,               \
    API_LIFECYCLE, CLK_LIFECYCLE, CPU_LIFECYCLE

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
