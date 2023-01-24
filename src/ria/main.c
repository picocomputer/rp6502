/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mon/cmd.h"
#include "mon/mon.h"
#include "ria.h"
#include "act.h"
#include "mon/rom.h"
#include "dev/com.h"
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

static void main_init()
{
    // Initialize UART for terminal
    com_init();

    // Hello, world.
    puts("\30\33[0m\f\n" RP6502_NAME);
    puts("\33[31mC\33[32mO\33[33mL\33[36mO\33[35mR\33[0m 64K RAM SYSTEM\n");

    // Interface Adapter to W65C02S
    ria_init();

    // TinyUSB host support for keyboards,
    // mice, joysticks, and storage devices.
    tusb_init();
    hid_init();

    lfs_init();
}

// These tasks run always, even when FatFs is blocking.
// Calling FatFs in here may cause undefined behavior.
void main_sys_tasks()
{
    tuh_task();
    hid_task();
    ria_task();
    act_task();
    com_task();
}

// These tasks do not run during FatFs IO.
// It is safe to call blocking FatFs operations.
static void main_app_tasks()
{
    mon_task();
    cmd_task();
    rom_task();
    // api_task(); //TODO
}

// This resets all modules and halts the 6502.
// It is called from CTRL-ALT-DEL and UART breaks.
// This may be called by main_sys_tasks so no FatFs calls.
void main_break()
{
    ria_stop();
    act_reset();
    com_reset();
    mon_reset();
    cmd_reset();
    rom_reset();
    puts("\30\33[0m");
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

    main_init();

    while (1)
    {
        main_sys_tasks();
        main_app_tasks();
    }

    return 0;
}
