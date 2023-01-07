/*
 * Copyright (c) 2022 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mon.h"
#include "ria.h"
#include "ria_action.h"
#include "ria_uart.h"
#include "hid.h"
#include "lfs.h"
#include "pico/stdlib.h"
#include "tusb.h"
#ifdef RASPBERRYPI_PICO_W
#include "pico/cyw43_arch.h"
#endif

#ifndef RP6502_NAME
#error RP6502_NAME must be defined
#endif

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

    // Initialize UART for terminal
    ria_uart_init();

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

    while (1)
    {
        tuh_task();
        hid_task();
        mon_task();
        ria_task();
        ria_action_task();
        ria_uart_task();
    }

    return 0;
}
