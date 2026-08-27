/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This machine's roster: the drivers it is made of, in the order it comes up.
 * core/lifecycle.c walks it -- forward for init and run, backward for stop.
 */

#include "core/lifecycle.h"

#include "core/api/api.h"
#include "core/api/clk.h"
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
#include <stdio.h>
#include "core/term/term.h"
#include "core/aud/aud.h"
#include "sw/apf.h"
#include "sw/cpu.h"
#include "sw/log.h"
#include "sw/fs.h"
#include "sw/rand.h"
#include "sw/vid.h"

/* vid first, so reversal puts vid_stop last: the display restore has to follow
 * everything that could still draw. fs before std, so reversal puts fs_stop
 * after std_stop -- std's closes are what park a read. */
#define ROSTER                                                  \
    VID_LIFECYCLE, LOG_LIFECYCLE, PROC_LIFECYCLE,               \
    AUD_LIFECYCLE, COM_LIFECYCLE, FS_LIFECYCLE,                \
    STD_LIFECYCLE, RLN_LIFECYCLE, TERM_LIFECYCLE,               \
    KEYBOARD_LIFECYCLE,                                         \
    KEYMAP_LIFECYCLE, MOUSE_LIFECYCLE, GAMEPAD_LIFECYCLE,       \
    TABLET_LIFECYCLE, APF_LIFECYCLE, TIM_LIFECYCLE,             \
    RAND_LIFECYCLE, API_LIFECYCLE, CLK_LIFECYCLE, CPU_LIFECYCLE

void lifecycle_init(void)
{
    cpu_init(); /* RESB low before anything else runs */
#define LIFECYCLE(i, r, s, b) i();
    LIFECYCLE_FORWARD(ROSTER)
#undef LIFECYCLE
    /* These two say so when an asset is missing, which a roster walk cannot,
     * and both self-heal on first use anyway. After the walk, so the console
     * they print to is up. */
    if (!unicode_init())
        printf("oem: no tables\n");
    if (!layout_init())
        printf("keyboard: no layouts\n");
    /* The canvas this selects calls term_set_height, so it needs the term the
     * walk laid out. */
    vid_init();
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
