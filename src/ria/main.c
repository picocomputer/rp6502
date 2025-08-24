/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "api/api.h"
#include "api/clk.h"
#include "api/oem.h"
#include "api/rng.h"
#include "api/std.h"
#include "aud/aud.h"
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
#include "usb/hid.h"
#include "usb/usb.h"
#include "usb/xin.h"

/**************************************/
/* All kernel modules register below. */
/**************************************/

// Many things are sensitive to order in obvious ways, like
// starting stdio before printing. Please list subtleties.

// Initialization event for power up, reboot command, or reboot button.
static void init(void)
{
    // Bring up system stdio first.
    com_init();

    // Configure the remaining GPIOs.
    cpu_init();
    ria_init();
    pix_init();
    vga_init();

    // Load config before we continue.
    lfs_init();
    cfg_init();

    // Print startup message after setting code page.
    oem_init();
    sys_init();

    // Misc kernel modules, add yours here.
    usb_init();
    led_init();
    aud_init();
    kbd_init();
    mou_init();
    pad_init();
    rom_init();
    clk_init();
    mdm_init();
}

// Tasks events are repeatedly called by the main kernel loop.
// They must not block. Use a state machine to do as
// much work as you can until something blocks.

// These tasks run when FatFs is blocking.
// Calling FatFs in here will summon a dragon.
void main_task(void)
{
    usb_task();
    cpu_task();
    ria_task();
    aud_task();
    kbd_task();
    cyw_task();
    vga_task();
    wfi_task();
    ntp_task();
    hid_task();
    xin_task();
    ble_task();
    led_task();
    com_task();
}

// Tasks that call FatFs should be here instead of main_task().
static void task(void)
{
    rln_task();
    api_task();
    mon_task();
    ram_task();
    fil_task();
    rom_task();
    mdm_task();
}

// Event to start running the 6502.
static void run(void)
{
    com_run();
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
    kbd_stop();
    mou_stop();
    pad_stop();
    aud_stop();
    mdm_stop();
}

// Event for CTRL-ALT-DEL and UART breaks.
static void reset(void)
{
    fil_reset();
    mon_reset();
    ram_reset();
    rom_reset();
    vga_reset();
    rln_reset();
}

// Triggered once after init then before every PHI2 clock change.
// Divider is used when PHI2 less than 4 MHz to
// maintain a minimum system clock of 128 MHz.
// From 4 to 8 MHz increases system clock to 256 MHz.
void main_pre_reclock(uint32_t sys_clk_khz, uint16_t clkdiv_int, uint8_t clkdiv_frac)
{
    (void)sys_clk_khz;
    (void)clkdiv_int;
    (void)clkdiv_frac;
    com_pre_reclock();
    cyw_pre_reclock();
}

// Triggered once after init then after every PHI2 clock change.
void main_post_reclock(uint32_t sys_clk_khz, uint16_t clkdiv_int, uint8_t clkdiv_frac)
{
    com_post_reclock();
    cpu_post_reclock();
    vga_post_reclock(sys_clk_khz);
    ria_post_reclock(clkdiv_int, clkdiv_frac);
    pix_post_reclock(clkdiv_int, clkdiv_frac);
    aud_post_reclock(sys_clk_khz);
    cyw_post_reclock(sys_clk_khz);
}

// PIX XREG writes to the RIA device will notify here.
bool main_pix(uint8_t ch, uint8_t addr, uint16_t word)
{
    (void)addr;
    switch (ch * 256 + addr)
    {
    case 0x000:
        return kbd_xreg(word);
    case 0x001:
        return mou_xreg(word);
    case 0x002:
        return pad_xreg(word);
    case 0x100:
        return psg_xreg(word);
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
        return oem_api_codepage();
    case 0x04:
        return rng_api_lrand();
    case 0x05:
        return com_api_stdin_opt();
    case 0x0F:
        return clk_api_clock();
    case 0x10:
        return clk_api_get_res();
    case 0x11:
        return clk_api_get_time();
    case 0x12:
        return clk_api_set_time();
    case 0x13:
        return clk_api_get_time_zone();
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
        return std_api_lseek();
    case 0x1B:
        return std_api_unlink();
    case 0x1C:
        return std_api_rename();
    }
    api_return_errno(API_ENOSYS);
    return false;
}

/*********************************/
/* This is the kernel scheduler. */
/*********************************/

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
    init();

    // Hack to start WiFi chip before changing clock
    cyw_task();

    // Trigger a reclock
    cpu_set_phi2_khz(cfg_get_phi2_khz());

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
            reset();
            is_breaking = false;
        }
    }
}
