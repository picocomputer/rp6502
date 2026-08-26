/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "core/sys/main.h"
#include "core/sys/proc.h"
#include "core/aud/aud_mix.h"
#include "core/dap/dbg.h"
#include "core/sys/msc.h"
#include "core/sys/rom.h"
#include "core/com/com.h"
#include "core/wdc/cpu.h"
#include "core/mem/mem.h"
#include "core/pix.h"
#include "core/vga/vga_emu.h"
#include "core/wdc/via.h"
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
#include "core/api/oem.h"
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

void main_init(void)
{
    mem_init();
    proc_init();
    cpu_init();
    aud_init();
    keyboard_init();
    mouse_init();
    gamepad_init();
    tablet_init();
    com_init();
    std_init();
    rln_init();
    tim_init();
    str_init();
    oem_init();
    font_init();
    term_init();
    vga_init();
}

/* The 6502 coming out of reset. */
void main_on_run(void)
{
    proc_run();
    com_run();
    rln_run();
    dir_run();
    api_run();
    clk_run();
    ria_run();
    via_run();
    cpu_run(); /* must be last */
}

/* The 6502 going into reset. */
void main_on_stop(void)
{
    cpu_stop(); /* must be first */
    vga_stop();
    rln_stop();
    api_stop();
    oem_stop(); /* a run-only code page belongs to the run that set it */
    std_stop();
    dir_stop();
    keyboard_stop();
    mouse_stop();
    gamepad_stop();
    tablet_stop();
    aud_stop();
}

/* Nowhere to break to: this machine has no monitor, and the key that asked is
 * an ordinary key. core/main.h documents false as that answer, so these satisfy
 * the contract rather than stub around it -- and without them emu_core cannot
 * link core/hid/keymap.c, which is the only caller. */
bool main_break(void)
{
    return false;
}

bool main_break_to_launcher(void)
{
    return false;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

const dir_backend_t *main_dir_backend(void)
{
    return &msc_dir_backend;
}

static const std_driver_t std_drivers[] = {
    {rom_std_handles, rom_std_open, rom_std_close, rom_std_read, NULL, NULL, rom_std_lseek},
    {msc_std_handles, msc_std_open, msc_std_close, msc_std_read, msc_std_write, msc_std_sync, msc_std_lseek},
};

const std_driver_t *main_std_drivers(size_t *count)
{
    *count = sizeof(std_drivers) / sizeof(std_drivers[0]);
    return std_drivers;
}
