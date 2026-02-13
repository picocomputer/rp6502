/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef RP6502_RIA_W
#include "net/wfi.h"
void wfi_task() {}
int wfi_status_response(char *, size_t, int) { return -1; }
#else

#include "net/cyw.h"
#include "net/wfi.h"
#include "str/str.h"
#include "sys/cfg.h"
#include <pico/cyw43_arch.h>

#if defined(DEBUG_RIA_NET) || defined(DEBUG_RIA_NET_WFI)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
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
static char wfi_ssid[WFI_SSID_SIZE];
static char wfi_pass[WFI_PASS_SIZE];

// Be aggressive 5 times then back off
#define WFI_RETRY_INITIAL_RETRIES 5
#define WFI_RETRY_INITIAL_SECS 2
#define WFI_RETRY_SECS 60

void wfi_shutdown(void)
{
    switch (wfi_state)
    {
    case wfi_state_connected:
    case wfi_state_connecting:
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        __attribute__((fallthrough));
    case wfi_state_connect:
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
        if (!cyw_get_rf_enable() || !wfi_ssid[0])
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
                     wfi_ssid, wfi_get_pass(),
                     strlen(wfi_get_pass()) ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN))
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
            wfi_retry_initial_retry_count = 0;
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
        if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) != CYW43_LINK_UP)
        {
            int secs = wfi_retry_connect();
            (void)secs;
            DBG("NET WFI connection lost, retry %ds\n", secs);
        }
        break;
    }
}

static const char *wifi_status_message(void)
{
    switch (wfi_state)
    {
    case wfi_state_off:
        if (!cyw_get_rf_enable())
            return STR_RF_OFF;
        else if (!wfi_ssid[0])
            return STR_WFI_NOT_CONFIGURED;
        else
            return STR_WFI_WAITING;
    case wfi_state_connect:
    case wfi_state_connecting:
        switch (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA))
        {
        case CYW43_LINK_JOIN:
            return STR_WFI_JOINING;
        case CYW43_LINK_NOIP:
            return STR_WFI_GETTING_IP;
        default:
            return STR_WFI_CONNECTING;
        }
    case wfi_state_connected:
        return STR_WFI_CONNECTED;
    case wfi_state_connect_failed:
        switch (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA))
        {
        case CYW43_LINK_NOIP:
            return STR_WFI_NO_IP_ADDRESS;
        case CYW43_LINK_NONET:
            return STR_WFI_SSID_NOT_FOUND;
        case CYW43_LINK_BADAUTH:
            return STR_WFI_AUTH_FAILED;
        default:
            return STR_WFI_CONNECT_FAILED;
        }
    }
    return STR_INTERNAL_ERROR;
}

int wfi_status_response(char *buf, size_t buf_size, int state)
{
    switch (state)
    {
    case 0:
    {
        snprintf(buf, buf_size, STR_STATUS_WIFI, wifi_status_message());
    }
    break;
    case 1:
    {
        uint8_t mac[6];
        cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac);
        snprintf(buf, buf_size, STR_STATUS_MAC,
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    break;
    case 2:
    {
        if (wfi_state == wfi_state_connected)
        {
            struct netif *netif = &cyw43_state.netif[CYW43_ITF_STA];
            const ip4_addr_t *ip4 = netif_ip4_addr(netif);
            if (!ip4_addr_isany_val(*ip4))
                snprintf(buf, buf_size, STR_STATUS_IPV4, ip4addr_ntoa(ip4));
        }
    }
    break;
    default:
        return -1;
    }
    return state + 1;
}

bool wfi_ready(void)
{
    return wfi_state == wfi_state_connected;
}

void wfi_load_ssid(const char *str, size_t len)
{
    str_parse_string(&str, &len, wfi_ssid, sizeof(wfi_ssid));
}

bool wfi_set_ssid(const char *ssid)
{
    size_t len = strlen(ssid);
    if (len < sizeof(wfi_ssid))
    {
        if (strcmp(wfi_ssid, ssid))
        {
            wfi_pass[0] = 0;
            strncpy(wfi_ssid, ssid, sizeof(wfi_ssid));
            wfi_shutdown();
            cfg_save();
        }
        return true;
    }
    return false;
}

const char *wfi_get_ssid(void)
{
    return wfi_ssid;
}

void wfi_load_pass(const char *str, size_t len)
{
    str_parse_string(&str, &len, wfi_pass, sizeof(wfi_pass));
}

bool wfi_set_pass(const char *pass)
{
    if (strlen(wfi_ssid) && strlen(pass) < sizeof(wfi_pass))
    {
        if (strcmp(wfi_pass, pass))
        {
            strncpy(wfi_pass, pass, sizeof(wfi_pass));
            wfi_shutdown();
            cfg_save();
        }
        return true;
    }
    return false;
}

const char *wfi_get_pass(void)
{
    return wfi_pass;
}

#endif /* RP6502_RIA_W */
