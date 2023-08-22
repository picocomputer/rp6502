/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "tusb.h"
#include "api/api.h"
#include "api/oem.h"
#include "api/rng.h"
#include "api/rtc.h"
#include "api/std.h"
#include "aud/aud.h"
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
#include "usb/hid.h"

/**************************************/
/* All kernel modules register below. */
/**************************************/

// Many things are sensitive to order in obvious ways, like
// starting the UART before printing. Please list subtleties.

// Initialization event for power up, reboot command, or reboot button.
static void init()
{
    // STDIO not available until after these inits
    cpu_init();
    ria_init();
    pix_init();
    vga_init();
    com_init();

    // Hello, world.
    puts("\30\33[0m\f");
    sys_init();

    // Load config before we continue
    lfs_init();
    cfg_init();

    // Misc kernel modules, add yours here
    oem_init();
    aud_init();
    hid_init();
    rom_init();
    led_init();
    rtc_init_();

    // TinyUSB
    tuh_init(TUH_OPT_RHPORT);
}

// Tasks events are repeatedly called by the main kernel loop.
// They must not block. Use a state machine to do as
// much work as you can until something blocks.

// These tasks run when FatFs is blocking.
// Calling FatFs in here may cause undefined behavior.
void main_task()
{
    tuh_task();
    cpu_task();
    ria_task();
    pix_task();
    aud_task();
    hid_task();
    vga_task();
    std_task();
}

// Tasks that call FatFs should be here instead of main_task().
static void task()
{
    api_task();
    com_task();
    mon_task();
    ram_task();
    fil_task();
    rom_task();
}

// Event to start running the 6502.
static void run()
{
    vga_run();
    api_run();
    ria_run(); // Must be immediately before cpu
    cpu_run(); // Must be last
}

// Event to stop the 6502.
static void stop()
{
    cpu_stop(); // Must be first
    ria_stop();
    pix_stop();
    std_stop();
}

// Event for CTRL-ALT-DEL and UART breaks.
static void reset()
{
    com_reset();
    fil_reset();
    mon_reset();
    ram_reset();
    rom_reset();
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
void main_pix(uint8_t ch, uint8_t byte, uint16_t word)
{
    switch (ch)
    {
    case 0:
        // usb_pix(byte, word); //TODO direct access to keyboard, mouse, gamepad
        break;
    case 1:
        aud_pix(byte, word);
        break;
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
        std_api_open();
        break;
    case 0x04:
        std_api_close();
        break;
    case 0x05:
        std_api_read_();
        break;
    case 0x06:
        std_api_readx();
        break;
    case 0x08:
        std_api_write_();
        break;
    case 0x09:
        std_api_writex();
        break;
    case 0x0B:
        std_api_lseek();
        break;
    case 0x10:
        pix_api_set_xreg();
        break;
    case 0x11:
        cpu_api_phi2();
        break;
    case 0x12:
        oem_api_codepage();
        break;
    case 0x13:
        rng_api_rand32();
        break;
    case 0x14:
        rtc_api_get_time();
        break;
    case 0x15:
        rtc_api_set_time();
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

void main_run()
{
    if (main_state != running)
        main_state = starting;
}

void main_stop()
{
    if (main_state == starting)
        main_state = stopped;
    if (main_state != stopped)
        main_state = stopping;
}

void main_break()
{
    is_breaking = true;
}

bool main_active()
{
    return main_state != stopped;
}

int main()
{
    // Delay a bit to wait for VGA initial power up.
    // Both UART Tx and Backchannel Tx characters of the
    // statup messages can get dropped if this isn't here.
    busy_wait_ms(10);

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

    return 0;
}
