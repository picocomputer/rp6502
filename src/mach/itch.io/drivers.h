/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This machine's drivers: the ones it is made of and the order it comes up
 * in, and the ones it offers a program to open. Both are the same kind of
 * fact, so they are the same file.
 *
 * It lives with the machine rather than in core because which drivers a
 * machine has is the one thing core cannot know. The software machines start
 * from the same list and are free to diverge. core/sys/sys.c walks the
 * machine rows; core/api/std.c builds the table from the stdio rows. The
 * drive a path reaches is in neither list: core/api/dir.h names those calls
 * and the host that is linked defines them.
 */

#ifndef _MACH_DRIVERS_H_
#define _MACH_DRIVERS_H_

#include "core/lifecycle.h"
#include "core/api/api.h"
#include "core/api/clk.h"
#include "core/api/dir.h"
#include "core/api/fs.h"
#include "core/api/proc.h"
#include "core/api/proc_exec.h"
#include "core/api/std.h"
#include "core/api/tim.h"
#include "core/aud/aud.h"
#include "core/com/com.h"
#include "core/hid/gamepad.h"
#include "core/hid/keyboard.h"
#include "core/hid/mouse.h"
#include "core/hid/tablet.h"
#include "core/mem/mem.h"
#include "core/ria/ria.h"
#include "core/rom/rom.h"
#include "core/str/oem.h"
#include "core/str/rln.h"
#include "core/str/str.h"
#include "core/term/font.h"
#include "core/term/term.h"
#include "core/vga/vga_emu.h"
#include "core/wdc/cpu.h"
#include "core/wdc/via.h"

/* init and run walk this forward; stop walks it backward. This machine has
 * no break fan-out -- there is no monitor to break into -- and it walks
 * neither task column: the frame loop in core/sys/sys.c is this machine's
 * scheduler, and every call site there says why it is where it is. */
#define RP6502_MACH_DRIVERS                                                  \
    RIA_MACH_LIFECYCLE, MEM_MACH_LIFECYCLE, PROC_EXEC_MACH_LIFECYCLE,        \
    PROC_MACH_LIFECYCLE, STR_MACH_LIFECYCLE,                                 \
    COM_MACH_LIFECYCLE, STD_MACH_LIFECYCLE, RLN_MACH_LIFECYCLE,              \
    TERM_MACH_LIFECYCLE, KEYBOARD_MACH_LIFECYCLE, MOUSE_MACH_LIFECYCLE,      \
    GAMEPAD_MACH_LIFECYCLE, TABLET_MACH_LIFECYCLE, FONT_MACH_LIFECYCLE,      \
    OEM_MACH_LIFECYCLE, VGA_MACH_LIFECYCLE, AUD_MACH_LIFECYCLE,              \
    TIM_MACH_LIFECYCLE, DIR_MACH_LIFECYCLE, API_MACH_LIFECYCLE,              \
    CLK_MACH_LIFECYCLE, VIA_MACH_LIFECYCLE, CPU_MACH_LIFECYCLE

/* What a program may open, in the order open() tries them. The filesystem is
 * the catch-all, so it is last. */
#define RP6502_STD_DRIVERS ROM_STD_LIFECYCLE, FS_STD_LIFECYCLE

#endif /* _MACH_DRIVERS_H_ */
