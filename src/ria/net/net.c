/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "pico.h"

#ifndef RASPBERRYPI_PICO2_W
void net_task() {}
void net_pre_reclock() {}
void net_post_reclock() {}
void net_reset_radio() {}
void net_print_status() {}
#else

#include "api/std.h"
#include "net/net.h"
#include "sys/cfg.h"
#include "sys/com.h"
#include "sys/vga.h"
#include "pico/cyw43_arch.h"
#include "pico/cyw43_driver.h"

// These are from cyw43_arch.h
// Change the help if you change these
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
    net_state_connect,
    net_state_connecting,
    net_state_connected,
    net_state_connect_failed,
} net_state_t;
net_state_t net_state;

bool net_led_status;
bool net_led_requested;

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
    switch (net_state)
    {
    case net_state_connect:
    case net_state_connected:
    case net_state_connect_failed:
    case net_state_connecting:
        cyw43_arch_disable_sta_mode();
        __attribute__((fallthrough));
    case net_state_initialized:
        cyw43_arch_deinit();
        net_state = net_state_off;
        break;
    case net_state_off:
    case net_state_init_failed:
        break;
    }
}

void net_task(void)
{
    switch (net_state)
    {
    case net_state_off:
        if (vga_active())
            break;
        // cyw43 driver blocks here while the cores boot
        // this prevents an awkward pause in the boot message
        com_flush();
        CYW43_NONE_PM;
        if (cyw43_arch_init_with_country(net_country_code()))
            net_state = net_state_init_failed;
        else
            net_state = net_state_initialized;
        break;
    case net_state_initialized:
        if (!cfg_get_ssid()[0])
            break;
        cyw43_arch_enable_sta_mode();
        net_state = net_state_connect;
        break;
    case net_state_connect:
        cyw43_arch_wifi_connect_async(
            cfg_get_ssid(), cfg_get_pass(),
            strlen(cfg_get_pass()) ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN);
        net_state = net_state_connecting;
        break;
    case net_state_connecting:
        int net_link_status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        switch (net_link_status)
        {
        case CYW43_LINK_DOWN:
        case CYW43_LINK_JOIN:
        case CYW43_LINK_NOIP:
            break;
        case CYW43_LINK_UP:
            net_state = net_state_connected;
            break;
        case CYW43_LINK_FAIL:
        case CYW43_LINK_NONET:
        case CYW43_LINK_BADAUTH:
            net_state = net_state_connect_failed;
            break;
        }
        break;
    case net_state_init_failed:
    case net_state_connect_failed:
    case net_state_connected:
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

void net_print_status(void)
{
    // print state
    printf("WiFi: ");
    switch (net_state)
    {
    case net_state_initialized:
        if (cfg_get_ssid()[0])
            puts("initialized");
        else
            puts("not configured");
        break;
    case net_state_connect:
    case net_state_connecting:
        puts("connecting");
        break;
    case net_state_connected:
        puts("connected");
        break;
    case net_state_connect_failed:
        switch (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA))
        {
        case CYW43_LINK_NOIP:
            puts("no IP address");
            break;
        case CYW43_LINK_NONET:
            puts("ssid not found");
            break;
        case CYW43_LINK_BADAUTH:
            puts("auth failed");
            break;
        default:
            puts("connect failed");
            break;
        }
        break;
    case net_state_off:
    case net_state_init_failed:
        puts("internal error");
        break;
    }

    // print MAC address
    uint8_t mac[6];
    cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac);
    printf("MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // print IP addresses
    if (net_state == net_state_connected)
    {
        struct netif *netif = &cyw43_state.netif[CYW43_ITF_STA];
        if (!ip4_addr_isany_val(*netif_ip4_addr(netif)))
        {
            const ip4_addr_t *ip4 = netif_ip4_addr(netif);
            printf("IPv4: %s\n", ip4addr_ntoa(ip4));
        }
#if LWIP_IPV6
        for (int i = 0; i < LWIP_IPV6_NUM_ADDRESSES; i++)
        {
            if (ip6_addr_isvalid(netif_ip6_addr_state(netif, i)))
            {
                const ip6_addr_t *ip6 = netif_ip6_addr(netif, i);
                printf("IPv6: %s\n", ip6addr_ntoa(ip6));
            }
        }
#endif
    }
}

bool net_ready(void)
{
    return net_state == net_state_connected;
}

void net_pre_reclock(void)
{
    net_reset_radio();
}

void net_post_reclock(uint32_t sys_clk_khz)
{
    // CYW43439 datasheet says 50MHz for SPI
    // It easily runs 85MHz+ so we push it to 66MHz
    if (sys_clk_khz > 198000)
        cyw43_set_pio_clkdiv_int_frac8(4, 0);
    else if (sys_clk_khz > 132000)
        cyw43_set_pio_clkdiv_int_frac8(3, 0);
    else
        cyw43_set_pio_clkdiv_int_frac8(2, 0);
}

#endif /* RASPBERRYPI_PICO2_W */
