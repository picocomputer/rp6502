/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "pico.h"

#ifndef RASPBERRYPI_PICO2_W
void net_init(void) {}
void net_task(void) {}
#else

#include "net/net.h"
#include "sys/cfg.h"
#include "sys/com.h"
#include "sys/vga.h"
#include "pico/cyw43_arch.h"

// These are from cyw43_arch.h
static const char COUNTRY_CODES[] = {
    'A', 'U', // AUSTRALIA
    'A', 'T', // AUSTRIA
    'B', 'E', // BELGIUM
    'B', 'R', // BRAZIL
    'C', 'A', // CANADA
    'C', 'L', // CHILE
    'C', 'N', // CHINA
    'C', 'O', // COLOMBIA
    'C', 'Z', // CZECH_REPUBLIC
    'D', 'K', // DENMARK
    'E', 'E', // ESTONIA
    'F', 'I', // FINLAND
    'F', 'R', // FRANCE
    'D', 'E', // GERMANY
    'G', 'R', // GREECE
    'H', 'K', // HONG_KONG
    'H', 'U', // HUNGARY
    'I', 'S', // ICELAND
    'I', 'N', // INDIA
    'I', 'L', // ISRAEL
    'I', 'T', // ITALY
    'J', 'P', // JAPAN
    'K', 'E', // KENYA
    'L', 'V', // LATVIA
    'L', 'I', // LIECHTENSTEIN
    'L', 'T', // LITHUANIA
    'L', 'U', // LUXEMBOURG
    'M', 'Y', // MALAYSIA
    'M', 'T', // MALTA
    'M', 'X', // MEXICO
    'N', 'L', // NETHERLANDS
    'N', 'Z', // NEW_ZEALAND
    'N', 'G', // NIGERIA
    'N', 'O', // NORWAY
    'P', 'E', // PERU
    'P', 'H', // PHILIPPINES
    'P', 'L', // POLAND
    'P', 'T', // PORTUGAL
    'S', 'G', // SINGAPORE
    'S', 'K', // SLOVAKIA
    'S', 'I', // SLOVENIA
    'Z', 'A', // SOUTH_AFRICA
    'K', 'R', // SOUTH_KOREA
    'E', 'S', // SPAIN
    'S', 'E', // SWEDEN
    'C', 'H', // SWITZERLAND
    'T', 'W', // TAIWAN
    'T', 'H', // THAILAND
    'T', 'R', // TURKEY
    'G', 'B', // UK
    'U', 'S', // USA
};

typedef enum
{
    net_state_off,
    net_state_initialized,
    net_state_init_failed,
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

bool net_validate_country_code(char *cc)
{
    if (!cc[0] || !cc[1] || cc[2] != 0)
        return false;
    for (size_t i = 0; i < sizeof(COUNTRY_CODES); i += 2)
        if (cc[0] == COUNTRY_CODES[i] && cc[1] == COUNTRY_CODES[i + 1])
            return true;
    return false;
}

static uint32_t net_country_code(void)
{
    const char *cc = cfg_get_rfcc();
    if (strlen(cc) == 2)
        return CYW43_COUNTRY(cc[0], cc[1], 0);
    else
        return CYW43_COUNTRY_WORLDWIDE;
}

void net_reset_radio(void)
{
    printf("reset radio\n"); ////////
    switch (net_state)
    {
    case net_state_connecting:
        cyw43_arch_disable_sta_mode();
        __attribute__((fallthrough));
    case net_state_initialized:
        cyw43_arch_deinit();
        __attribute__((fallthrough));
    case net_state_off:
    case net_state_init_failed:
    case net_state_connected:
    case net_state_connect_failed:
        break;
    }
    net_state = net_state_off;
}

void __not_in_flash_func(net_task)(void)
{
    switch (net_state)
    {
    case net_state_off:
        if (vga_active() || !cfg_get_ssid()[0])
            break;
        // cyw43 driver blocks here while the cores boot
        // this prevents an awkward pause in the boot message
        com_flush();
        net_error = cyw43_arch_init_with_country(net_country_code());
        if (net_error)
            net_state = net_state_init_failed;
        else
            net_state = net_state_initialized;
        assert(net_error == 0);
        printf("init %d\n", net_error); ////////
        break;
    case net_state_initialized:
        cyw43_arch_enable_sta_mode();
        net_state = net_state_connecting;
        printf("sta_mode %d\n", net_error); ////////
        break;
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
