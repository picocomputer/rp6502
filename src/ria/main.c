/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "tusb.h"
#include "api/api.h"
#include "api/clk.h"
#include "api/oem.h"
#include "api/rng.h"
#include "api/std.h"
#include "aud/aud.h"
#include "aud/psg.h"
#include "mon/fil.h"
#include "mon/mon.h"
#include "mon/ram.h"
#include "mon/rom.h"
#include "sys/com.h"
#include "sys/cfg.h"
#include "sys/cpu.h"
#include "sys/led.h"
#include "sys/lfs.h"
#include "sys/pix.h"
#include "sys/ria.h"
#include "sys/sys.h"
#include "sys/vga.h"
#include "usb/kbd.h"
#include "usb/mou.h"
#include "usb/pad.h"

/**************************************/
/* All kernel modules register below. */
/**************************************/

// Many things are sensitive to order in obvious ways, like
// starting the UART before printing. Please list subtleties.

// Initialization event for power up, reboot command, or reboot button.
static void init(void)
{
    // STDIO not available until after these inits
    cpu_init();
    ria_init();
    pix_init();
    vga_init();
    com_init();

    // Print startup message
    sys_init();

    // Load config before we continue
    lfs_init();
    cfg_init();

    // Misc kernel modules, add yours here
    oem_init();
    aud_init();
    kbd_init();
    mou_init();
    pad_init();
    rom_init();
    led_init();
    clk_init();

    // TinyUSB
    tuh_init(TUH_OPT_RHPORT);
}

// Tasks events are repeatedly called by the main kernel loop.
// They must not block. Use a state machine to do as
// much work as you can until something blocks.

// These tasks run when FatFs is blocking.
// Calling FatFs in here may cause undefined behavior.
void main_task(void)
{
    tuh_task();
    cpu_task();
    ria_task();
    aud_task();
    kbd_task();
    pad_task();
    vga_task();
    std_task();
}

// Tasks that call FatFs should be here instead of main_task().
static void task(void)
{
    api_task();
    com_task();
    mon_task();
    ram_task();
    fil_task();
    rom_task();
}

// Event to start running the 6502.
static void run(void)
{
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
    ria_stop();
    pix_stop();
    std_stop();
    kbd_stop();
    mou_stop();
    pad_stop();
    aud_stop();
}

// Event for CTRL-ALT-DEL and UART breaks.
static void reset(void)
{
    com_reset();
    fil_reset();
    mon_reset();
    ram_reset();
    rom_reset();
    vga_reset();
}

// Triggered once after init then after every PHI2 clock change.
// Divider is used when PHI2 less than 4 MHz to
// maintain a minimum system clock of 120 MHz.
// From 4 to 8 MHz increases system clock to 240 MHz.
void main_reclock(uint32_t sys_clk_khz, uint16_t clkdiv_int, uint8_t clkdiv_frac)
{
    com_reclock();
    cpu_reclock();
    vga_reclock(sys_clk_khz);
    ria_reclock(clkdiv_int, clkdiv_frac);
    pix_reclock(clkdiv_int, clkdiv_frac);
    aud_reclock(sys_clk_khz);
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

// This will repeatedly trigger until API_BUSY is false so
// IO operations can hold busy while waiting for data.
// Be sure any state is reset in a stop() handler.
bool main_api(uint8_t operation)
{
    switch (operation)
    {
    case 0x01:
        pix_api_xreg();
        break;
    case 0x02:
        cpu_api_phi2();
        break;
    case 0x03:
        oem_api_codepage();
        break;
    case 0x04:
        rng_api_lrand();
        break;
    case 0x05:
        cpu_api_stdin_opt();
        break;
    case 0x0F:
        clk_api_clock();
        break;
    case 0x10:
        clk_api_get_res();
        break;
    case 0x11:
        clk_api_get_time();
        break;
    case 0x12:
        clk_api_set_time();
        break;
    case 0x14:
        std_api_open();
        break;
    case 0x15:
        std_api_close();
        break;
    case 0x16:
        std_api_read_xstack();
        break;
    case 0x17:
        std_api_read_xram();
        break;
    case 0x18:
        std_api_write_xstack();
        break;
    case 0x19:
        std_api_write_xram();
        break;
    case 0x1A:
        std_api_lseek();
        break;
    case 0x1B:
        std_api_unlink();
        break;
    case 0x1C:
        std_api_rename();
        break;
    default:
        return false;
    }
    return true;
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

    // Trigger main_reclock()
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
