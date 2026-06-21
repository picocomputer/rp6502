/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef RP6502_RIA_W
#include "net/wfi.h"
void wfi_task() {}
int wfi_status_response(char *, size_t, int, unsigned) { return -1; }
int wfi_scan_response(char *, size_t, int, unsigned) { return -1; }
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

static int wfi_retry_count;
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
    wfi_retry_count = 0;
}

static void wfi_retry_connect(void)
{
    int secs = wfi_retry_count < WFI_RETRY_INITIAL_RETRIES
                   ? WFI_RETRY_INITIAL_SECS
                   : WFI_RETRY_SECS;
    wfi_state = wfi_state_connect_failed;
    wfi_retry_timer = make_timeout_time_ms(secs * 1000);
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
}

void wfi_task(void)
{
    switch (wfi_state)
    {
    case wfi_state_off:
        if (!cyw_get_rf_enable() || !wfi_ssid[0])
            break;
        cyw43_arch_enable_sta_mode();
        wfi_state = wfi_state_connect;
        break;
    case wfi_state_connect:
        DBG("NET WFI connecting\n");
        // Power management may be buggy, turn it off
        if (cyw43_wifi_pm(&cyw43_state, CYW43_DEFAULT_PM & ~0xf))
            wfi_retry_connect();
        else if (cyw43_arch_wifi_connect_async(
                     wfi_ssid, wfi_get_pass(),
                     strlen(wfi_get_pass()) ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN))
            wfi_retry_connect();
        else
            wfi_state = wfi_state_connecting;
        break;
    case wfi_state_connecting:
        switch (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA))
        {
        case CYW43_LINK_DOWN:
        case CYW43_LINK_JOIN:
        case CYW43_LINK_NOIP:
            break;
        case CYW43_LINK_UP:
            DBG("NET WFI connected\n");
            wfi_retry_count = 0;
            wfi_state = wfi_state_connected;
            break;
        case CYW43_LINK_FAIL:
        case CYW43_LINK_NONET:
        case CYW43_LINK_BADAUTH:
            DBG("NET WFI connect failed\n");
            wfi_retry_connect();
            break;
        }
        break;
    case wfi_state_connect_failed:
        if (time_reached(wfi_retry_timer))
        {
            wfi_retry_count++;
            wfi_state = wfi_state_connect;
        }
        break;
    case wfi_state_connected:
        if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) != CYW43_LINK_UP)
        {
            DBG("NET WFI connection lost\n");
            wfi_retry_connect();
        }
        break;
    }
}

static const char *wfi_status_message(void)
{
    switch (wfi_state)
    {
    case wfi_state_off:
        if (!cyw_get_rf_enable())
            return S(STR_RF_OFF);
        else if (!wfi_ssid[0])
            return S(STR_WFI_NOT_CONFIGURED);
        else
            return S(STR_WFI_WAITING);
    case wfi_state_connect:
    case wfi_state_connecting:
        switch (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA))
        {
        case CYW43_LINK_JOIN:
            return S(STR_WFI_JOINING);
        case CYW43_LINK_NOIP:
            return S(STR_WFI_GETTING_IP);
        default:
            return S(STR_WFI_CONNECTING);
        }
    case wfi_state_connected:
        return S(STR_WFI_CONNECTED);
    case wfi_state_connect_failed:
        switch (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA))
        {
        case CYW43_LINK_NOIP:
            return S(STR_WFI_NO_IP_ADDRESS);
        case CYW43_LINK_NONET:
            return S(STR_WFI_SSID_NOT_FOUND);
        case CYW43_LINK_BADAUTH:
            return S(STR_WFI_AUTH_FAILED);
        default:
            return S(STR_WFI_CONNECT_FAILED);
        }
    }
    return S(STR_INTERNAL_ERROR);
}

int wfi_status_response(char *buf, size_t buf_size, int state, unsigned)
{
    switch (state)
    {
    case 0:
    {
        int32_t rssi;
        if (!cyw43_wifi_get_rssi(&cyw43_state, &rssi) && rssi != 0)
            snprintf_utf8(buf, buf_size, STR_STATUS_WIFI_RSSI,
                          wfi_status_message(), (int)rssi);
        else
            snprintf_utf8(buf, buf_size, STR_STATUS_WIFI, wfi_status_message());
    }
    break;
    case 1:
    {
        uint8_t mac[6];
#if RP6502_CREATOR
        mac[0] = 0xBA;
        mac[1] = 0xDC;
        mac[2] = 0x0F;
        mac[3] = 0xFE;
        mac[4] = 0xEB;
        mac[5] = 0xAD;
#else
        cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac);
#endif
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

// WiFi scan: a small store of the strongest unique access points, refreshed on
// each wfi_scan_response invocation.
#define WFI_SCAN_MAX 24
#define WFI_SCAN_TIMEOUT_MS 5000

typedef struct
{
    char ssid[33];
    int8_t rssi;
    uint8_t auth; // scan auth bitmask: bit0 WEP, bit1 WPA, bit2 WPA2/RSN
} wfi_ap_t;

static wfi_ap_t wfi_aps[WFI_SCAN_MAX];
static uint8_t wfi_ap_count;
static enum { WFI_SCAN_IDLE, WFI_SCAN_BUSY, WFI_SCAN_DONE } wfi_scan_status;
static absolute_time_t wfi_scan_deadline;

// Insert sorted by RSSI descending, dedup by SSID keeping the strongest.
static void wfi_ap_insert(const char *ssid, int8_t rssi, uint8_t auth)
{
    for (unsigned i = 0; i < wfi_ap_count; i++)
        if (!strcmp(wfi_aps[i].ssid, ssid))
        {
            if (rssi > wfi_aps[i].rssi)
            {
                wfi_aps[i].rssi = rssi;
                wfi_aps[i].auth = auth;
            }
            return;
        }
    unsigned idx;
    if (wfi_ap_count < WFI_SCAN_MAX)
        idx = wfi_ap_count++;
    else if (rssi > wfi_aps[WFI_SCAN_MAX - 1].rssi)
        idx = WFI_SCAN_MAX - 1;
    else
        return;
    strncpy(wfi_aps[idx].ssid, ssid, sizeof(wfi_aps[idx].ssid) - 1);
    wfi_aps[idx].ssid[sizeof(wfi_aps[idx].ssid) - 1] = 0;
    wfi_aps[idx].rssi = rssi;
    wfi_aps[idx].auth = auth;
    while (idx > 0 && wfi_aps[idx].rssi > wfi_aps[idx - 1].rssi)
    {
        wfi_ap_t tmp = wfi_aps[idx - 1];
        wfi_aps[idx - 1] = wfi_aps[idx];
        wfi_aps[idx] = tmp;
        idx--;
    }
}

static int wfi_scan_cb(void *env, const cyw43_ev_scan_result_t *r)
{
    (void)env;
    if (r->ssid_len == 0)
        return 0; // hidden network
    char ssid[33];
    unsigned n = r->ssid_len > 32 ? 32 : r->ssid_len;
    for (unsigned k = 0; k < n; k++)
    {
        uint8_t ch = r->ssid[k];
        ssid[k] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '.';
    }
    ssid[n] = 0;
    wfi_ap_insert(ssid, (int8_t)r->rssi, r->auth_mode);
    return 0;
}

static void wfi_scan_begin(void)
{
    if (wfi_scan_status == WFI_SCAN_BUSY)
        return;
    wfi_ap_count = 0;
    cyw43_arch_enable_sta_mode();
    cyw43_wifi_scan_options_t opts = {0};
    if (!cyw43_wifi_scan(&cyw43_state, &opts, NULL, wfi_scan_cb))
    {
        wfi_scan_status = WFI_SCAN_BUSY;
        wfi_scan_deadline = make_timeout_time_ms(WFI_SCAN_TIMEOUT_MS);
    }
    else
        wfi_scan_status = WFI_SCAN_DONE;
}

static bool wfi_scan_busy(void)
{
    if (wfi_scan_status != WFI_SCAN_BUSY)
        return false;
    if (cyw43_wifi_scan_active(&cyw43_state) && !time_reached(wfi_scan_deadline))
        return true;
    wfi_scan_status = WFI_SCAN_DONE;
    return false;
}

static void wfi_scan_format(unsigned i, char *buf, size_t size)
{
    const wfi_ap_t *ap = &wfi_aps[i];
    const char *sec = (ap->auth & 4) ? "WPA2"
                      : (ap->auth & 2) ? "WPA"
                      : (ap->auth & 1) ? "WEP"
                                       : "OPEN";
    snprintf(buf, size, "%4ddBm  %-4s  %s\n", ap->rssi, sec, ap->ssid);
}

int wfi_scan_response(char *buf, size_t buf_size, int state, unsigned width)
{
    (void)width; // single column
    if (state < 0)
        return state;
    if (!cyw_get_rf_enable())
    {
        if (state == 0)
            snprintf_utf8(buf, buf_size, "%s\n", S(STR_RF_OFF));
        return -1;
    }
    if (state == 0)
    {
        if (!wfi_scan_busy())
            wfi_scan_begin();
        return 1;
    }
    if (wfi_scan_busy())
    {
        buf[0] = 0; // not ready; call again
        return 1;
    }
    unsigned i = (unsigned)state - 1;
    if (i >= wfi_ap_count)
    {
        if (i == 0)
            snprintf_utf8(buf, buf_size, "%s\n", S(STR_WFI_NO_NETWORKS));
        return -1;
    }
    wfi_scan_format(i, buf, buf_size);
    return state + 1;
}

bool wfi_ready(void)
{
    return wfi_state == wfi_state_connected;
}

bool wfi_connecting(void)
{
    return wfi_state == wfi_state_connect ||
           wfi_state == wfi_state_connecting ||
           (wfi_state == wfi_state_connect_failed &&
            wfi_retry_count < WFI_RETRY_INITIAL_RETRIES);
}

void wfi_load_ssid(const char *str)
{
    size_t n = strlen(str);
    if (n < sizeof(wfi_ssid))
    {
        memcpy(wfi_ssid, str, n);
        wfi_ssid[n] = 0;
    }
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

void wfi_load_pass(const char *str)
{
    size_t n = strlen(str);
    if (n < sizeof(wfi_pass))
    {
        memcpy(wfi_pass, str, n);
        wfi_pass[n] = 0;
    }
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
