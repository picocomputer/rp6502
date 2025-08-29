/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef RP6502_RIA_W
#include "net/wfi.h"
void wfi_task() {}
void wfi_print_status() {}
#else

#include "net/cyw.h"
#include "net/wfi.h"
#include "sys/cfg.h"
#include <pico/cyw43_arch.h>

#if defined(DEBUG_RIA_NET) || defined(DEBUG_RIA_NET_WFI)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

typedef enum
{
    wfi_state_off,
    wfi_state_connect,
    wfi_state_connecting,
    wfi_state_connected,
    wfi_state_connect_failed,
} wfi_state_t;
static wfi_state_t wfi_state;

static int wfi_retry_initial_retry_count;
static absolute_time_t wfi_retry_timer;

// Be aggressive 5 times then back off
#define WFI_RETRY_INITIAL_RETRIES 5
#define WFI_RETRY_INITIAL_SECS 2
#define WFI_RETRY_SECS 60

void wfi_shutdown(void)
{
    switch (wfi_state)
    {
    case wfi_state_connect:
    case wfi_state_connected:
    case wfi_state_connecting:
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        __attribute__((fallthrough));
    case wfi_state_connect_failed:
        cyw43_arch_disable_sta_mode();
        wfi_state = wfi_state_off;
        __attribute__((fallthrough));
    case wfi_state_off:
        break;
    }
    wfi_retry_initial_retry_count = 0;
}

static int wfi_retry_connect(void)
{
    int secs = wfi_retry_initial_retry_count < WFI_RETRY_INITIAL_RETRIES
                   ? WFI_RETRY_INITIAL_SECS
                   : WFI_RETRY_SECS;
    wfi_state = wfi_state_connect_failed;
    wfi_retry_timer = make_timeout_time_ms(secs * 1000);
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    return secs;
}

void wfi_task(void)
{
    switch (wfi_state)
    {
    case wfi_state_off:
        if (!cfg_get_rf() || !cfg_get_ssid()[0])
            break;
        cyw43_arch_enable_sta_mode(); // cyw43_wifi_set_up
        wfi_state = wfi_state_connect;
        break;
    case wfi_state_connect:
        DBG("NET WFI connecting\n");
        // Power management may be buggy, turn it off
        if (cyw43_wifi_pm(&cyw43_state, CYW43_DEFAULT_PM & ~0xf))
        {
            int secs = wfi_retry_connect();
            (void)secs;
            DBG("NET WFI cyw43_wifi_pm failed, retry %ds\n", secs);
        }
        else if (cyw43_arch_wifi_connect_async(
                     cfg_get_ssid(), cfg_get_pass(),
                     strlen(cfg_get_pass()) ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN))
        {
            int secs = wfi_retry_connect();
            (void)secs;
            DBG("NET WFI cyw43_arch_wifi_connect_async failed, retry %ds\n", secs);
        }
        else
            wfi_state = wfi_state_connecting;
        break;
    case wfi_state_connecting:
        int link_status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        switch (link_status)
        {
        case CYW43_LINK_DOWN:
        case CYW43_LINK_JOIN:
        case CYW43_LINK_NOIP:
            break;
        case CYW43_LINK_UP:
            DBG("NET WFI connected\n");
            wfi_state = wfi_state_connected;
            break;
        case CYW43_LINK_FAIL:
        case CYW43_LINK_NONET:
        case CYW43_LINK_BADAUTH:
            int secs = wfi_retry_connect();
            (void)secs;
            DBG("NET WFI connect failed (%d), retry %ds\n", link_status, secs);
            break;
        }
        break;
    case wfi_state_connect_failed:
        if (absolute_time_diff_us(get_absolute_time(), wfi_retry_timer) < 0)
        {
            wfi_retry_initial_retry_count++;
            wfi_state = wfi_state_connect;
        }
        break;
    case wfi_state_connected:
        break;
    }
}

void wfi_print_status(void)
{
    // print state
    printf("WiFi: ");
    switch (wfi_state)
    {
    case wfi_state_off:
        if (!cfg_get_rf())
            puts("radio off");
        else if (!cfg_get_ssid()[0])
            puts("not configured");
        else
            puts("waiting");
        break;
    case wfi_state_connect:
    case wfi_state_connecting:
        printf("connecting");
        switch (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA))
        {
        case CYW43_LINK_JOIN:
            puts(", joining");
            break;
        case CYW43_LINK_NOIP:
            puts(", getting IP");
            break;
        case CYW43_LINK_DOWN:
        default:
            puts("");
            break;
        }
        break;
    case wfi_state_connected:
        puts("connected");
        break;
    case wfi_state_connect_failed:
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
    default:
        puts("internal error");
        break;
    }

    // print MAC address
    uint8_t mac[6];
    cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac);
    printf("MAC : %02X:%02X:%02X:%02X:%02X:%02X\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // print IP addresses
    if (wfi_state == wfi_state_connected)
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

bool wfi_ready(void)
{
    return wfi_state == wfi_state_connected;
}

#endif /* RP6502_RIA_W */
