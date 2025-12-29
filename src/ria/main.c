/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "api/api.h"
#include "api/clk.h"
#include "api/dir.h"
#include "api/oem.h"
#include "api/rng.h"
#include "api/std.h"
#include "aud/aud.h"
#include "aud/opl.h"
#include "aud/psg.h"
#include "hid/kbd.h"
#include "hid/mou.h"
#include "hid/pad.h"
#include "mon/fil.h"
#include "mon/mon.h"
#include "mon/ram.h"
#include "mon/rom.h"
#include "net/ble.h"
#include "net/cyw.h"
#include "net/mdm.h"
#include "net/ntp.h"
#include "net/wfi.h"
#include "sys/com.h"
#include "sys/cfg.h"
#include "sys/cpu.h"
#include "sys/led.h"
#include "sys/lfs.h"
#include "sys/pix.h"
#include "sys/ria.h"
#include "sys/rln.h"
#include "sys/sys.h"
#include "sys/vga.h"
#include "usb/usb.h"
#include "usb/xin.h"

/**************************************/
/* All device drivers register below. */
/**************************************/

// Many things are sensitive to order in obvious ways, like
// starting stdio before printing. Please list subtleties.

// Initialization event for power up, reboot command, or reboot button.
static void init(void)
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
    cyw_init();
    oem_init();
    usb_init();
    led_init();
    aud_init();
    kbd_init();
    mou_init();
    pad_init();
    rom_init();
    clk_init();
    mdm_init();

    // CPU must be last. Triggers a reclock.
    cpu_init();
}

// Task events are repeatedly called by the main loop.
// They must not block. All drivers are state machines.

// These tasks run while FatFs is blocking.
// Calling FatFs in here will summon a dragon.
void main_task(void)
{
    usb_task();
    cpu_task();
    ria_task();
    kbd_task();
    cyw_task();
    vga_task();
    com_task();
    wfi_task();
    ntp_task();
    xin_task();
    ble_task();
    led_task();
    mdm_task();
    ram_task();
}

// Tasks that call FatFs should be here instead of main_task().
static void task(void)
{
    mon_task();
    api_task();
    rln_task();
    fil_task();
    rom_task();
}

// Event to start running the 6502.
static void run(void)
{
    com_run();
    std_run();
    dir_run();
    vga_run();
    api_run();
    clk_run();
    ria_run(); // Must be immediately before cpu
    cpu_run(); // Must be last
}

// Event to stop the 6502.
static void stop(void)
{
    cpu_stop(); // Must be first
    vga_stop(); // Must be before ria
    com_stop();
    api_stop();
    ria_stop();
    pix_stop();
    oem_stop();
    std_stop();
    dir_stop();
    kbd_stop();
    mou_stop();
    pad_stop();
    aud_stop();
    mdm_stop();
}

// Event for CTRL-ALT-DEL and UART breaks.
// Stop will be executed first if 6502 is running.
static void break_(void) // break is keyword
{
    fil_break();
    mon_break();
    ram_break();
    rom_break();
    vga_break();
    rln_break();
}

// Triggered once after init then after every PHI2 change.
void main_reclock(uint16_t clkdiv_int, uint8_t clkdiv_frac)
{
    cpu_reclock();
    ria_reclock(clkdiv_int, clkdiv_frac);
    pix_reclock(clkdiv_int, clkdiv_frac);
}

// PIX XREG writes to the RIA device will dispatch here.
bool main_xreg(uint8_t chan, uint8_t addr, uint16_t word)
{
    (void)addr;
    switch (chan * 256 + addr)
    {
    // Channel 0 for human interface devices.
    case 0x000:
        return kbd_xreg(word);
    case 0x001:
        return mou_xreg(word);
    case 0x002:
        return pad_xreg(word);
    // Channel 1 for audio devices.
    case 0x100:
        return psg_xreg(word);
    case 0x101:
        return opl_xreg(word);
    default:
        return false;
    }
}

// API call implementations should return true if they have more
// work to process. They will be called repeatedly until returning
// false. Be sure any state is reset in a stop() handler.
bool main_api(uint8_t operation)
{
    switch (operation)
    {
    case 0x01:
        return pix_api_xreg();
    case 0x02:
        return cpu_api_phi2();
    case 0x03:
        return oem_api_code_page();
    case 0x04:
        return rng_api_lrand();
    case 0x05:
        return std_api_stdin_opt();
    case 0x06:
        return api_api_errno_opt();
    case 0x0D:
        return clk_api_tzset();
    case 0x0E:
        return clk_api_tzquery();
    case 0x0F:
        return clk_api_clock();
    case 0x10:
        return clk_api_get_res();
    case 0x11:
        return clk_api_get_time();
    case 0x12:
        return clk_api_set_time();
    case 0x13: // ok to reuse
        break; // retired clk_api_get_time_zone
    case 0x14:
        return std_api_open();
    case 0x15:
        return std_api_close();
    case 0x16:
        return std_api_read_xstack();
    case 0x17:
        return std_api_read_xram();
    case 0x18:
        return std_api_write_xstack();
    case 0x19:
        return std_api_write_xram();
    case 0x1A:
        return std_api_lseek_cc65();
    case 0x1B:
        return dir_api_unlink();
    case 0x1C:
        return dir_api_rename();
    case 0x1D:
        return std_api_lseek_llvm();
    case 0x1E:
        return std_api_syncfs();
    case 0x1F:
        return dir_api_stat();
    case 0x20:
        return dir_api_opendir();
    case 0x21:
        return dir_api_readdir();
    case 0x22:
        return dir_api_closedir();
    case 0x23:
        return dir_api_telldir();
    case 0x24:
        return dir_api_seekdir();
    case 0x25:
        return dir_api_rewinddir();
    case 0x26:
        return dir_api_chmod();
    case 0x27:
        return dir_api_utime();
    case 0x28:
        return dir_api_mkdir();
    case 0x29:
        return dir_api_chdir();
    case 0x2A:
        return dir_api_chdrive();
    case 0x2B:
        return dir_api_getcwd();
    case 0x2C:
        return dir_api_setlabel();
    case 0x2D:
        return dir_api_getlabel();
    case 0x2E:
        return dir_api_getfree();
    }
    return api_return_errno(API_ENOSYS);
}

/*****************************/
/* This is the OS scheduler. */
/*****************************/

static bool is_breaking;
static enum state {
    stopped,
    starting,
    running,
    stopping,
} volatile main_state;

void main_run(void)
{
    if (main_state != running)
        main_state = starting;
}

void main_stop(void)
{
    if (main_state == starting)
        main_state = stopped;
    if (main_state != stopped)
        main_state = stopping;
}

void main_break(void)
{
    is_breaking = true;
}

bool main_active(void)
{
    return main_state != stopped;
}

int main(void)
{
    cpu_main();
    init();
    while (true)
    {
        main_task();
        task();
        if (is_breaking)
        {
            if (main_state == starting)
                main_state = stopped;
            if (main_state == running)
                main_state = stopping;
        }
        if (main_state == starting)
        {
            run();
            main_state = running;
        }
        if (main_state == stopping)
        {
            stop();
            main_state = stopped;
        }
        if (is_breaking)
        {
            break_();
            is_breaking = false;
        }
    }
}
