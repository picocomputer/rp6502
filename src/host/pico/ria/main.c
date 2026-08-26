/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ria/main.h"
#include "core/api/api.h"
#include "core/api/attr.h"
#include "core/api/clk.h"
#include "api/fat.h"
#include "core/api/dir.h"
#include "core/api/oem.h"
#include "core/api/proc.h"
#include "core/api/std.h"
#include "core/api/tim.h"
#include "core/aud/aud.h"
#include "core/aud/opl.h"
#include "core/aud/psg.h"
#include "ria/ble/ble.h"
#include "core/hid/keyboard.h"
#include "core/hid/keymap.h"
#include "core/hid/mouse.h"
#include "core/hid/pad.h"
#include "core/hid/tab.h"
#include "ria/mon/drive.h"
#include "ria/mon/fil.h"
#include "ria/mon/mon.h"
#include "ria/mon/ram.h"
#include "ria/mon/rom.h"
#include "ria/mon/uf2.h"
#include "ria/net/cyw.h"
#include "ria/net/modem.h"
#include "ria/net/ntp.h"
#include "ria/net/wifi.h"
#include "core/str/rln.h"
#include "core/str/str.h"
#include "ria/sys/com.h"
#include "ria/sys/cfg.h"
#include "ria/sys/cpu.h"
#include "ria/sys/led.h"
#include "ria/sys/lfs.h"
#include "ria/sys/mem.h"
#include "ria/sys/pix.h"
#include "ria/sys/ria.h"
#include "ria/sys/sys.h"
#include "ria/sys/vga.h"
#include "ria/usb/usb.h"
#include "ria/usb/mid.h"
#include "ria/usb/nfc.h"
#include "ria/usb/vcp.h"
#include "ria/usb/xin.h"
#include <pico/stdlib.h>
#include <pico/time.h>
#include <stdio.h>

#ifndef NDEBUG
#define TIME_TASK(fn)                                                  \
    do                                                                 \
    {                                                                  \
        absolute_time_t _t0 = get_absolute_time();                     \
        fn();                                                          \
        int64_t _us = absolute_time_diff_us(_t0, get_absolute_time()); \
        if (_us > 10000)                                               \
            printf("SLOW " #fn " %lldus\n", (long long)_us);           \
    } while (0)
#else
#define TIME_TASK(fn) fn()
#endif

/**************************************/
/* All device drivers register below. */
/**************************************/

// Driver table, msc is catch-all and must be last.
const dir_backend_t *main_dir_backend(void)
{
    return &fat_dir_backend;
}

static __in_flash("std_drivers") const std_driver_t std_drivers[] = {
    {modem_std_handles, modem_std_open, modem_std_close, modem_std_read, modem_std_write, NULL, NULL},
    {vcp_std_handles, vcp_std_open, vcp_std_close, vcp_std_read, vcp_std_write, NULL, NULL},
    {mid_std_handles, mid_std_open, mid_std_close, mid_std_read, mid_std_write, mid_std_sync, NULL},
    {rom_std_handles, rom_std_open, rom_std_close, rom_std_read, NULL, NULL, rom_std_lseek},
    {nfc_std_handles, nfc_std_open, nfc_std_close, nfc_std_read, nfc_std_write, NULL, NULL},
    {fat_std_handles, fat_std_open, fat_std_close, fat_std_read, fat_std_write, fat_std_sync, fat_std_lseek},
};

const std_driver_t *main_std_drivers(size_t *count)
{
    *count = sizeof(std_drivers) / sizeof(std_drivers[0]);
    return std_drivers;
}

// Many things are sensitive to order in obvious ways, like
// starting stdio before printing. Please list subtleties.

// Initialization event for power up, reboot command, or reboot button.
static void __in_flash("init") init(void)
{
    // Bring up stdio dispatcher first for DBG().
    com_init();

    // Queue startup message.
    sys_init();

    // GPIO drivers.
    ria_init();
    pix_init();
    vga_init(); // Must be after PIX

    // Load config before we continue.
    lfs_init();
    cfg_init(); // Config stored on lfs

    // Misc device drivers, add yours here.
    str_init();
    std_init();
    cyw_init();
    oem_init();
    led_init();
    aud_init();
    keyboard_init();
    keymap_init(); /* the speller is this machine's, not the device layer's */
    mouse_init();
    pad_init();
    tab_init();
    rom_init();
    tim_init();
    modem_init();
    rln_init();

    // USB near end for boot enum timing
    usb_init();

    // CPU must be last. Triggers a reclock.
    cpu_init();
}

// Task events are repeatedly called by the main loop.
// They must not block. All drivers are state machines.

// These tasks run while FatFs is blocking.
// Calling FatFs in here will summon a dragon.
void main_task(void)
{
    TIME_TASK(usb_task);
    std_task();
    cpu_task();
    ria_task();
    keymap_task();
    mid_task();
    cyw_task();
    vga_task();
    com_task();
    wifi_task();
    ntp_task();
    ble_task();
    led_task();
    modem_task();
    ram_task();
}

// Tasks that call FatFs should be here instead of main_task().
static void task(void)
{
    mon_task();
    mem_task();
    rln_task();
    fil_task();
    rom_task();
    uf2_task();
    vcp_task();
    nfc_task(); // must be last for exec
    api_task(); // must be last for exec
}

// Event to start running the 6502.
void main_on_run(void)
{
    proc_run();
    com_run();
    rln_run();
    dir_run();
    vga_run();
    api_run();
    clk_run();
    ria_run(); // Must be immediately before cpu
    cpu_run(); // Must be last
}

// Event to stop the 6502.
void main_on_stop(void)
{
    cpu_stop(); // Must be first
    vga_stop();
    rln_stop();
    api_stop();
    pix_stop();
    oem_stop();
    std_stop();
    mid_stop();
    dir_stop();
    keyboard_stop();
    mouse_stop();
    pad_stop();
    tab_stop();
    aud_stop();
    modem_stop();
    rom_stop();
    proc_stop();
    mon_stop();
    com_stop(); // Adds newline
    ria_stop(); // Last for stops that check ria_active()
}

// Event for CTRL-ALT-DEL and UART breaks.
// Stop will be executed first if 6502 is running.
static void break_(void) // break is keyword
{
    drive_break();
    fil_break();
    mon_break();
    ram_break();
    rom_break();
    vga_break();
    mem_break();
    rln_break();
    com_break();
}

// Triggered once after init then after every PHI2 change.
void main_reclock(uint16_t clkdiv_int, uint8_t clkdiv_frac)
{
    cpu_reclock();
    ria_reclock(clkdiv_int, clkdiv_frac);
    pix_reclock(clkdiv_int, clkdiv_frac);
}


/*****************************/
/* This is the OS scheduler. */
/*****************************/

static bool is_breaking;

bool main_break(void)
{
    proc_cancel_launcher();
    is_breaking = true;
    return true;
}

bool main_break_to_launcher(void)
{
    // From the launcher there is nowhere to return to.
    if (proc_is_launcher())
        return false;
    api_set_ax(0xFFFF);
    is_breaking = true;
    return true;
}

int main(void)
{
    sys_main();
    cpu_main();
    init();
    while (true)
    {
        main_task();
        task();
        if (is_breaking)
            main_stop();
        main_commit();
        if (is_breaking)
        {
            break_();
            is_breaking = false;
        }
    }
}
