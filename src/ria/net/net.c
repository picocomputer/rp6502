/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "pico.h"

#ifndef RASPBERRYPI_PICO2_W
void net_init(void) {}
void net_task(void) {}
void net_led(void) {}
#else

#include "net/net.h"
#include "sys/com.h"
#include "sys/vga.h"
#include "pico/cyw43_arch.h"

// The country code bullshit makes a lot of the cyw43 library
// initialization useless, so it's replicated here without all the bullshit.
// For fucks sake, just expose setting it without needing t

typedef enum
{
    net_state_off,
    net_state_initialized,
    net_state_init_failed,
    net_state_ready,
    net_state_connecting,
    net_state_connected,
    net_state_connect_failed,
} net_state_t;
net_state_t net_state;
int net_error;

bool net_led_status;
bool net_led_requested;

void net_init(void)
{
    // net_task();
}

void net_task(void)
{
    switch (net_state)
    {
    case net_state_off:
        if (vga_active())
            break;
        com_flush();
        net_error = cyw43_arch_init_with_country(PICO_CYW43_ARCH_DEFAULT_COUNTRY_CODE);
        if (net_error)
            net_state = net_state_init_failed;
        else
            net_state = net_state_initialized;
        assert(net_error == 0);
        // printf("init %d\n", net_error); ////////
        break;
    case net_state_initialized:
        cyw43_arch_enable_sta_mode();
        net_state = net_state_ready;
        // printf("sta_mode %d\n", net_error); ////////
    default:
        break;
    }

    switch (net_state)
    {
    case net_state_off:
    case net_state_init_failed:
        break;
    default:
        if (net_led_requested != net_led_status)
        {
            net_led_status = net_led_requested;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, net_led_status);
        }
        cyw43_arch_poll();
    }
}

void net_led(bool ison)
{
    net_led_requested = ison;
}

#endif /* RASPBERRYPI_PICO2_W */
