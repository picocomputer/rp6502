/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This machine's roster: the drivers it is made of, in the order it comes up.
 * core/lifecycle.c walks it -- forward to bring up, backward to tear down.
 *
 * It lives with the machine rather than in core because which drivers a
 * machine has is the one thing core cannot know. The software machines start
 * from the same list and are free to diverge.
 */

#include "core/lifecycle.h"
#include "core/api/proc_exec.h"
#include "core/aud/aud_mix.h"
#include "core/dap/dbg.h"
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

/* What this machine is made of, in the order it comes up. init and run walk
 * this forward; stop walks it backward. This machine has no break fan-out --
 * there is no monitor to break into. */
#define ROSTER                                                 \
    RIA_MACH_LIFECYCLE, MEM_MACH_LIFECYCLE, PROC_EXEC_MACH_LIFECYCLE,           \
    PROC_MACH_LIFECYCLE, STR_MACH_LIFECYCLE,                             \
    COM_MACH_LIFECYCLE, STD_MACH_LIFECYCLE, RLN_MACH_LIFECYCLE,               \
    TERM_MACH_LIFECYCLE, KEYBOARD_MACH_LIFECYCLE, MOUSE_MACH_LIFECYCLE,       \
    GAMEPAD_MACH_LIFECYCLE, TABLET_MACH_LIFECYCLE, FONT_MACH_LIFECYCLE,       \
    OEM_MACH_LIFECYCLE, VGA_MACH_LIFECYCLE, AUD_MACH_LIFECYCLE,               \
    TIM_MACH_LIFECYCLE, DIR_MACH_LIFECYCLE, API_MACH_LIFECYCLE,               \
    CLK_MACH_LIFECYCLE, VIA_MACH_LIFECYCLE, CPU_MACH_LIFECYCLE

void lifecycle_init(void)
{
#define LIFECYCLE(i, t, iot, r, s, b) i();
    LIFECYCLE_FORWARD(ROSTER)
#undef LIFECYCLE
}

/* The 6502 coming out of reset. */
void lifecycle_on_run(void)
{
#define LIFECYCLE(i, t, iot, r, s, b) r();
    LIFECYCLE_FORWARD(ROSTER)
#undef LIFECYCLE
}

/* Going back into it. */
void lifecycle_on_stop(void)
{
#define LIFECYCLE(i, t, iot, r, s, b) s();
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
