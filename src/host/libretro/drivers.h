/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_DRIVERS_H_
#define _HOST_DRIVERS_H_

#include "core/sys/driver.h"
#include "core/api/api.h"
#include "core/api/xreg.h"
#include "core/sys/random.h"
#include "core/api/clk.h"
#include "core/api/dir.h"
#include "osal/fs.h"
#include "core/api/proc.h"
#include "core/sys/proc.h"
#include "core/api/std.h"
#include "core/api/tim.h"
#include "core/aud/mix.h"
#include "core/aud/opl.h"
#include "core/aud/psg.h"
#include "core/com/com.h"
#include "core/hid/gamepad.h"
#include "core/hid/vtkeys.h"
#include "core/hid/keyboard.h"
#include "core/hid/mouse.h"
#include "core/hid/tablet.h"
#include "core/wdc/sram.h"
#include "core/sys/xram.h"
#include "core/ria/ria.h"
#include "core/rom/rom.h"
#include "core/str/oem.h"
#include "core/str/rln.h"
#include "core/str/str.h"
#include "core/term/font.h"
#include "core/term/term.h"
#include "core/vga/vga_emu.h"
#include "core/wdc/bus.h"
#include "core/wdc/cpu.h"
#include "core/wdc/via.h"
#include "core/wdc/phi2.h"

/* VGA comes before BUS because vga_task advances the beam one scanline and
 * bus_task then runs the 6502 up to it. TERM comes before VGA because
 * vga_init programs the console canvas, which sets the terminal's height. */
#define RP6502_MACH_DRIVERS                                                  \
    RIA_DRIVER, SRAM_DRIVER, XRAM_DRIVER,                     \
    PROC_DRIVER, STR_DRIVER, ASSET_DRIVER,\
    COM_DRIVER, STD_DRIVER, RLN_DRIVER,              \
    API_DRIVER, XREG_DRIVER, TERM_DRIVER,                                 \
    KEYBOARD_DRIVER, MOUSE_DRIVER,                           \
    GAMEPAD_DRIVER, TABLET_DRIVER, FONT_DRIVER,      \
    OEM_DRIVER, VGA_DRIVER, VTKEYS_DRIVER,           \
    PSG_DRIVER, OPL_DRIVER, AUD_DRIVER, TIM_DRIVER, DIR_DRIVER,              \
    CLK_DRIVER, RANDOM_DRIVER, PHI2_DRIVER,                 \
    CPU_DRIVER, VIA_DRIVER, BUS_DRIVER

#define RP6502_STD_DRIVERS ROM_STD_DRIVER, FS_STD_DRIVER

/* No host attaches a serial port to this machine, so the only bytes that ever
 * reach the UART source are the terminal's replies to a program's queries,
 * which core/com/com.c promotes into that ring. */
#define RP6502_COM_SOURCES                       \
    [COM_SOURCE_KEYBOARD] = COM_KEYBOARD_SOURCE, \
    [COM_SOURCE_UART] = COM_UART_SOURCE

#endif /* _HOST_DRIVERS_H_ */
