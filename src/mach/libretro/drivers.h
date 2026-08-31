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
 * from the same list and are free to diverge. core/sys.c walks the
 * machine rows; core/api/std.c builds the table from the stdio rows. The
 * drive a path reaches is in neither list: core/api/dir.h names those calls
 * and the host that is linked defines them.
 */

#ifndef _MACH_DRIVERS_H_
#define _MACH_DRIVERS_H_

#include "core/driver.h"
#include "core/api/api.h"
#include "core/api/clk.h"
#include "core/api/dir.h"
#include "core/api/fs.h"
#include "core/api/proc.h"
#include "core/sys/exec.h"
#include "core/api/std.h"
#include "core/api/tim.h"
#include "core/aud/aud.h"
#include "core/com/com.h"
#include "core/hid/gamepad.h"
#include "core/hid/vtkeys.h"
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

/* init and run walk this forward; stop walks it backward; the two task
 * columns are walked forward every pass of core/sys.c's sys_task and
 * sys_io_task, which with sys_commit are this machine's super-loop. There
 * is no break fan-out -- no monitor to break into.
 *
 * Video leads and the CPU follows, so VGA sits before CPU: the beam advances
 * a scanline and cpu_task runs the 6502 up to it. TERM stays after API in the
 * io column (its lazy clears drain a row per call) and before VGA in the list
 * (vga_init programs the console canvas, which asks term its height). VIA
 * before CPU: they share RESB. */
#define RP6502_MACH_DRIVERS                                                  \
    RIA_DRIVER, MEM_DRIVER, EXEC_DRIVER,        \
    PROC_DRIVER, STR_DRIVER,                                 \
    COM_DRIVER, STD_DRIVER, RLN_DRIVER,              \
    API_DRIVER, TERM_DRIVER,                                 \
    KEYBOARD_DRIVER, MOUSE_DRIVER,                           \
    GAMEPAD_DRIVER, TABLET_DRIVER, FONT_DRIVER,      \
    OEM_DRIVER, VGA_DRIVER, VTKEYS_DRIVER,           \
    AUD_DRIVER, TIM_DRIVER, DIR_DRIVER,              \
    CLK_DRIVER, VIA_DRIVER, CPU_DRIVER

/* What a program may open, in the order open() tries them. The filesystem is
 * the catch-all, so it is last. */
#define RP6502_STD_DRIVERS ROM_STD_DRIVER, FS_STD_DRIVER

#endif /* _MACH_DRIVERS_H_ */
