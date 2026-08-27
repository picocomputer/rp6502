/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This machine's roster: the drivers it is made of, in the order it comes up.
 * core/lifecycle.c walks it -- forward for init and run, backward for stop.
 *
 * It lives with the machine rather than in core because which drivers a
 * machine has is the one thing core cannot know. The software machines start
 * from the same list and are free to diverge.
 */

#include "core/lifecycle.h"
#include "core/api/proc_exec.h"
#include "core/aud/aud_mix.h"
#include "core/dap/dbg.h"
#include "core/sys/msc.h"
#include "core/rom/rom.h"
#include "core/com/com.h"
#include "core/wdc/cpu.h"
#include "core/mem/mem.h"
#include "core/pix.h"
#include "core/vga/vga_emu.h"
#include "core/wdc/via.h"
#include "core/hid/hid.h"
#include "core/hid/keyboard.h"
#include "core/hid/mouse.h"
#include "core/hid/gamepad.h"
#include "core/hid/tablet.h"
#include "core/ria/ria.h"
#include "core/api/api.h"
#include "core/api/attr.h"
#include "core/api/dir.h"
#include "core/api/std.h"
#include "core/api/clk.h"
#include "core/str/oem.h"
#include "core/api/tim.h"
#include "core/aud/aud.h"
#include "core/aud/psg.h"
#include "core/aud/opl.h"
#include "core/str/rln.h"
#include "core/str/str.h"
#include "core/term/font.h"
#include "core/term/term.h"
#include "core/vga/mode0.h"
#include "core/vga/mode1.h"
#include "core/vga/mode2.h"
#include "core/vga/mode3.h"
#include "core/vga/mode4.h"
#include "core/vga/mode5.h"
#include <stdio.h>
#include <string.h>

/* What this machine is made of, in the order it comes up. init, run and
 * break walk this forward; stop walks it backward. */
#define ROSTER                                                 \
    RIA_LIFECYCLE, MEM_LIFECYCLE, SYS_PROC_LIFECYCLE,           \
    PROC_LIFECYCLE, STR_LIFECYCLE,                             \
    COM_LIFECYCLE, STD_LIFECYCLE, RLN_LIFECYCLE,               \
    TERM_LIFECYCLE, KEYBOARD_LIFECYCLE, MOUSE_LIFECYCLE,       \
    GAMEPAD_LIFECYCLE, TABLET_LIFECYCLE, FONT_LIFECYCLE,       \
    OEM_LIFECYCLE, VGA_LIFECYCLE, AUD_LIFECYCLE,               \
    TIM_LIFECYCLE, DIR_LIFECYCLE, API_LIFECYCLE,               \
    CLK_LIFECYCLE, VIA_LIFECYCLE, CPU_LIFECYCLE

void lifecycle_init(void)
{
#define LIFECYCLE(i, r, s, b) i();
    LIFECYCLE_FORWARD(ROSTER)
#undef LIFECYCLE
}

/* The 6502 coming out of reset. */
void lifecycle_on_run(void)
{
#define LIFECYCLE(i, r, s, b) r();
    LIFECYCLE_FORWARD(ROSTER)
#undef LIFECYCLE
}

/* Going back into it. */
void lifecycle_on_stop(void)
{
#define LIFECYCLE(i, r, s, b) s();
    LIFECYCLE_REVERSE(ROSTER)
#undef LIFECYCLE
}

bool lifecycle_break(void)
{
    return false;
}

bool lifecycle_break_to_launcher(void)
{
    return false;
}
