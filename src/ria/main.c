/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "act.h"
#include "api.h"
#include "cfg.h"
#include "cpu.h"
#include "pix.h"
#include "ria.h"
#include "mon/fil.h"
#include "mon/mon.h"
#include "mon/rom.h"
#include "mon/sys.h"
#include "dev/aud.h"
#include "dev/com.h"
#include "dev/hid.h"
#include "dev/lfs.h"
#include "dev/rng.h"
#include "dev/std.h"
#include "tusb.h"
#include "pico/stdlib.h"
#ifdef RASPBERRYPI_PICO_W
#include "pico/cyw43_arch.h"
#endif

#ifndef RP6502_NAME
#error RP6502_NAME must be defined
#endif

static bool is_breaking;
static enum state {
    stopped,
    starting,
    running,
    stopping,
} volatile main_state;

// Run the 6502
void main_run()
{
    if (main_state != running)
        main_state = starting;
}

// Stop the 6502
void main_stop()
{
    if (main_state != stopped)
        main_state = stopping;
}

// A break is triggered by CTRL-ALT-DEL and UART breaks.
void main_break()
{
    is_breaking = true;
}

/*************************************/
/* All kernel modules register here. */
/*************************************/

// Many things are sensitive to order in obvious ways, like
// starting the UART before printing. Please list subtleties.

// Only called once.
static void init()
{
    // Initialize UART for terminal
    com_init();

    // Hello, world.
    puts("\30\33[0m\f\n" RP6502_NAME);
    puts("64K RAM, 64K XRAM");
    puts("16-bit \33[31mC\33[32mO\33[33mL\33[36mO\33[35mR\33[0m VGA\n");

    // Load config before we init anything
    lfs_init();
    cfg_init();

    // Misc kernel modules, add yours here
    cpu_init();
    tusb_init();
    aud_init();
    hid_init();
    ria_init();
    act_init();
    pix_init();

    // This triggers main_reclock()
    cpu_set_phi2_khz(cfg_get_phi2_khz());

    // mbuf has boot string from cfg_init()
    size_t mbuf_len = strlen((char *)mbuf);
    if (mbuf_len)
        rom_load_lfs((char *)mbuf, mbuf_len);
}

// This is called to start the 6502.
static void run()
{
    api_run();
    act_run(); // After api_run, may override REGS $FFF0-F9
    cpu_run(); // Must be last
}

// This is called to stop the 6502.
static void stop()
{
    cpu_stop(); // Must be first
    act_stop();
    pix_stop();
    std_stop();
}

// This is called by CTRL-ALT-DEL and UART breaks.
static void reset()
{
    com_reset();
    fil_reset();
    mon_reset();
    sys_reset();
    rom_reset();
    puts("\30\33[0m");
}

// Called before any clock change.
void main_preclock()
{
    com_preclock();
}

// Called once during init then after every clock change.
void main_reclock(uint32_t phi2_khz, uint32_t sys_clk_khz, uint16_t clkdiv_int, uint8_t clkdiv_frac)
{
    (void)phi2_khz;
    (void)sys_clk_khz;
    com_reclock();
    ria_reclock(clkdiv_int, clkdiv_frac);
    act_reclock(clkdiv_int, clkdiv_frac);
    pix_reclock(clkdiv_int, clkdiv_frac);
}

// These tasks run always, even when FatFs is blocking.
// Calling FatFs in here may cause undefined behavior.
void main_task()
{
    cpu_task();
    ria_task();
    act_task();
    com_task();
    aud_task();
    tuh_task();
    hid_task();
}

// Tasks that call FatFs should be here instead of main_task().
static void task()
{
    mon_task();
    sys_task();
    fil_task();
    rom_task();
    api_task();
}

// PIX writes to the RIA will notify here.
void main_pix(uint8_t ch, uint8_t byte, uint16_t word)
{
    switch (ch)
    {
    case 0:
        aud_pix(byte, word);
        break;
    }
}

// This will repeatedly call until API_BUSY is false
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
        cfg_api_codepage();
        break;
    case 0x13:
        rng_api_rand32();
        break;
    default:
        return false;
    }
    return true;
}

/*********************************/
/* This is the kernel scheduler. */
/*********************************/

int main()
{
    // Pi Pico LED on.
#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);
#endif
#ifdef RASPBERRYPI_PICO_W
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
#endif

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
            reset();
            is_breaking = false;
        }
    }

    return 0;
}
