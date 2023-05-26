/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mon/sys.h"
#include "mon/fil.h"
#include "mon/mon.h"
#include "ria.h"
#include "aud.h"
#include "api.h"
#include "act.h"
#include "api.h"
#include "cfg.h"
#include "cpu.h"
#include "mon/rom.h"
#include "dev/com.h"
#include "mem/mbuf.h"
#include "hid.h"
#include "dev/lfs.h"
#include "pico/stdlib.h"
#include "tusb.h"
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
    if (main_state == starting)
        main_state = stopped;
    if (main_state == running)
        main_state = stopping;
    is_breaking = true;
}

// Device drivers that require initialization should register here.
// Many things are sensitive to order in obvious ways, like starting
// the UART before printing. Please list subtleties here.
static void init()
{
    // Initialize UART for terminal
    com_init();

    // Hello, world.
    puts("\30\33[0m\f\n" RP6502_NAME);
    puts("64K RAM, 64K XRAM");
    puts("16-bit \33[31mC\33[32mO\33[33mL\33[36mO\33[35mR\33[0m VGA\n");

    // Internal systems
    aud_init();
    tusb_init();
    hid_init();
    lfs_init();

    // Loading config before we init PHI2 so it will
    // initially start at user defined speed.
    cfg_load();

    // 6502 systems
    cpu_init();
    ria_init();
    act_init();

    // mbuf has boot string from cfg_load()
    size_t mbuf_len = strlen((char *)mbuf);
    if (mbuf_len)
        rom_load_lfs((char *)mbuf, mbuf_len);
}

// This is called to start the 6502.
static void run()
{
    api_run();
    act_run();
    cpu_run();
}

// This is called to stop the 6502.
static void stop()
{
    cpu_stop();
    act_stop();
    api_stop();
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
        if (main_state == stopping)
        {
            stop();
            main_state = stopped;
        }
        if (main_state == stopped && is_breaking)
        {
            reset();
            is_breaking = false;
        }
        if (main_state == starting)
        {
            run();
            main_state = running;
        }
    }

    return 0;
}
