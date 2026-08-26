/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef RP6502_RIA_W
#include "ria/net/wifi.h"
void wifi_task() {}
int wifi_status_response(char *, size_t, int, unsigned) { return -1; }
int wifi_scan_response(char *, size_t, int, unsigned) { return -1; }
#else

#include "ria/net/cyw.h"
#include "ria/net/wifi.h"
#include "core/str/str.h"
#include "ria/sys/com.h"
#include "ria/sys/cfg.h"
#include "ria/sys/mem.h"
#include <pico/cyw43_arch.h>

#if defined(DEBUG_RIA_NET) || defined(DEBUG_RIA_NET_WIFI)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

typedef enum
{
    wifi_state_off,
    wifi_state_connect,
    wifi_state_connecting,
    wifi_state_connected,
    wifi_state_connect_failed,
} wifi_state_t;
static wifi_state_t wifi_state;

static int wifi_retry_count;
static absolute_time_t wifi_retry_timer;
static char wifi_ssid[WIFI_SSID_SIZE];
static char wifi_pass[WIFI_PASS_SIZE];

// Be aggressive 5 times then back off
#define WIFI_RETRY_INITIAL_RETRIES 5
#define WIFI_RETRY_INITIAL_SECS 2
#define WIFI_RETRY_SECS 60

void wifi_shutdown(void)
{
    switch (wifi_state)
    {
    case wifi_state_connected:
    case wifi_state_connecting:
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        __attribute__((fallthrough));
    case wifi_state_connect:
    case wifi_state_connect_failed:
        cyw43_arch_disable_sta_mode();
        wifi_state = wifi_state_off;
        __attribute__((fallthrough));
    case wifi_state_off:
        break;
    }
    wifi_retry_count = 0;
}

static void wifi_retry_connect(void)
{
    int secs = wifi_retry_count < WIFI_RETRY_INITIAL_RETRIES
                   ? WIFI_RETRY_INITIAL_SECS
                   : WIFI_RETRY_SECS;
    wifi_state = wifi_state_connect_failed;
    wifi_retry_timer = make_timeout_time_ms(secs * 1000);
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
}

void wifi_task(void)
{
    switch (wifi_state)
    {
    case wifi_state_off:
        if (!cyw_get_rf_enable() || !wifi_ssid[0])
            break;
        cyw43_arch_enable_sta_mode();
        wifi_state = wifi_state_connect;
        break;
    case wifi_state_connect:
        DBG("NET WIFI connecting\n");
        // Power management may be buggy, turn it off
        if (cyw43_wifi_pm(&cyw43_state, CYW43_DEFAULT_PM & ~0xf))
            wifi_retry_connect();
        else if (cyw43_arch_wifi_connect_async(
                     wifi_ssid, wifi_get_pass(),
                     strlen(wifi_get_pass()) ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN))
            wifi_retry_connect();
        else
            wifi_state = wifi_state_connecting;
        break;
    case wifi_state_connecting:
        switch (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA))
        {
        case CYW43_LINK_DOWN:
        case CYW43_LINK_JOIN:
        case CYW43_LINK_NOIP:
            break;
        case CYW43_LINK_UP:
            DBG("NET WIFI connected\n");
            wifi_retry_count = 0;
            wifi_state = wifi_state_connected;
            break;
        case CYW43_LINK_FAIL:
        case CYW43_LINK_NONET:
        case CYW43_LINK_BADAUTH:
            DBG("NET WIFI connect failed\n");
            wifi_retry_connect();
            break;
        }
        break;
    case wifi_state_connect_failed:
        if (time_reached(wifi_retry_timer))
        {
            wifi_retry_count++;
            wifi_state = wifi_state_connect;
        }
        break;
    case wifi_state_connected:
        if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) != CYW43_LINK_UP)
        {
            DBG("NET WIFI connection lost\n");
            wifi_retry_connect();
        }
        break;
    }
}

static const char *wifi_status_message(void)
{
    switch (wifi_state)
    {
    case wifi_state_off:
        if (!cyw_get_rf_enable())
            return S(STR_RF_OFF);
        else if (!wifi_ssid[0])
            return S(STR_WIFI_NOT_CONFIGURED);
        else
            return S(STR_WIFI_WAITING);
    case wifi_state_connect:
    case wifi_state_connecting:
        switch (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA))
        {
        case CYW43_LINK_JOIN:
            return S(STR_WIFI_JOINING);
        case CYW43_LINK_NOIP:
            return S(STR_WIFI_GETTING_IP);
        default:
            return S(STR_WIFI_CONNECTING);
        }
    case wifi_state_connected:
        return S(STR_WIFI_CONNECTED);
    case wifi_state_connect_failed:
        switch (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA))
        {
        case CYW43_LINK_NOIP:
            return S(STR_WIFI_NO_IP_ADDRESS);
        case CYW43_LINK_NONET:
            return S(STR_WIFI_SSID_NOT_FOUND);
        case CYW43_LINK_BADAUTH:
            return S(STR_WIFI_AUTH_FAILED);
        default:
            return S(STR_WIFI_CONNECT_FAILED);
        }
    }
    return S(STR_INTERNAL_ERROR);
}

int wifi_status_response(char *buf, size_t buf_size, int state, unsigned)
{
    switch (state)
    {
    case 0:
    {
        int32_t rssi;
        if (!cyw43_wifi_get_rssi(&cyw43_state, &rssi) && rssi != 0)
            com_snprintf_utf8(buf, buf_size, STR_STATUS_WIFI_RSSI,
                              wifi_status_message(), (int)rssi);
        else
            com_snprintf_utf8(buf, buf_size, STR_STATUS_WIFI, wifi_status_message());
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
        if (wifi_state == wifi_state_connected)
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

// WiFi scan store: strongest unique AP per SSID. Overlaid on mbuf instead of a
// static buffer; valid only across one monitor scan/render cycle (6502 halted,
// async wifi_scan_cb the sole writer).
#define WIFI_SCAN_MAX 24

typedef struct
{
    char ssid[33];
    int8_t rssi;
    uint8_t auth; // scan auth bitmask: bit0 WEP, bit1 WPA, bit2 WPA2/RSN
} wifi_ap_t;

_Static_assert(WIFI_SCAN_MAX * sizeof(wifi_ap_t) <= MBUF_SIZE,
               "wifi scan store exceeds mbuf");

static uint8_t wifi_ap_count;
static enum { WIFI_SCAN_IDLE,
              WIFI_SCAN_BUSY,
              WIFI_SCAN_DONE } wifi_scan_status;

// Record the latest non-zero RSSI per unique SSID, in discovery order.
static void wifi_ap_insert(const char *ssid, int8_t rssi, uint8_t auth)
{
    wifi_ap_t *aps = (wifi_ap_t *)mbuf;
    if (rssi == 0)
        return;
    for (unsigned i = 0; i < wifi_ap_count; i++)
        if (!strcmp(aps[i].ssid, ssid))
        {
            aps[i].rssi = rssi;
            aps[i].auth = auth;
            return;
        }
    if (wifi_ap_count >= WIFI_SCAN_MAX)
        return;
    unsigned idx = wifi_ap_count++;
    strncpy(aps[idx].ssid, ssid, sizeof(aps[idx].ssid) - 1);
    aps[idx].ssid[sizeof(aps[idx].ssid) - 1] = 0;
    aps[idx].rssi = rssi;
    aps[idx].auth = auth;
}

static int wifi_scan_cb(void *env, const cyw43_ev_scan_result_t *r)
{
    (void)env;
    DBG("NET WIFI scan t=%u %02x:%02x:%02x:%02x:%02x:%02x rssi=%4d auth=%02x len=%2u ssid=%.*s\n",
        (unsigned)to_ms_since_boot(get_absolute_time()),
        r->bssid[0], r->bssid[1], r->bssid[2], r->bssid[3], r->bssid[4], r->bssid[5],
        r->rssi, r->auth_mode, r->ssid_len,
        (int)(r->ssid_len > 32 ? 32 : r->ssid_len), (const char *)r->ssid);
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
    wifi_ap_insert(ssid, (int8_t)r->rssi, r->auth_mode);
    return 0;
}

static void wifi_scan_begin(void)
{
    if (wifi_scan_status == WIFI_SCAN_BUSY)
        return;
    wifi_ap_count = 0;
    cyw43_arch_enable_sta_mode();
    cyw43_wifi_scan_options_t opts = {0};
    if (!cyw43_wifi_scan(&cyw43_state, &opts, NULL, wifi_scan_cb))
    {
        wifi_scan_status = WIFI_SCAN_BUSY;
        DBG("NET WIFI scan begin t=%u\n", (unsigned)to_ms_since_boot(get_absolute_time()));
    }
    else
    {
        DBG("NET WIFI scan begin FAILED t=%u\n", (unsigned)to_ms_since_boot(get_absolute_time()));
        wifi_scan_status = WIFI_SCAN_DONE;
    }
}

static bool wifi_scan_busy(void)
{
    if (wifi_scan_status != WIFI_SCAN_BUSY)
        return false;
    if (cyw43_wifi_scan_active(&cyw43_state))
        return true;
    DBG("NET WIFI scan done  t=%u count=%u\n",
        (unsigned)to_ms_since_boot(get_absolute_time()), wifi_ap_count);
    wifi_scan_status = WIFI_SCAN_DONE;
    return false;
}

// Sort by RSSI, strongest first.
static void wifi_ap_sort(void)
{
    wifi_ap_t *aps = (wifi_ap_t *)mbuf;
    for (unsigned i = 1; i < wifi_ap_count; i++)
    {
        wifi_ap_t key = aps[i];
        unsigned j = i;
        while (j > 0 && aps[j - 1].rssi < key.rssi)
        {
            aps[j] = aps[j - 1];
            j--;
        }
        aps[j] = key;
    }
}

static void wifi_scan_format(unsigned i, char *buf, size_t size)
{
    const wifi_ap_t *ap = &((wifi_ap_t *)mbuf)[i];
    const char *sec = (ap->auth & 4)   ? "WPA2"
                      : (ap->auth & 2) ? "WPA"
                      : (ap->auth & 1) ? "WEP"
                                       : "OPEN";
    snprintf(buf, size, "%4ddBm  %-4s  %s\n", ap->rssi, sec, ap->ssid);
}

int wifi_scan_response(char *buf, size_t buf_size, int state, unsigned width)
{
    (void)width; // single column
    if (state < 0)
        return state;
    if (!cyw_get_rf_enable())
    {
        if (state == 0)
            com_snprintf_utf8(buf, buf_size, "%s\n", S(STR_RF_OFF));
        return -1;
    }
    if (state == 0)
    {
        if (!wifi_scan_busy())
            wifi_scan_begin();
        return 1;
    }
    if (wifi_scan_busy())
    {
        buf[0] = 0; // not ready; call again
        return 1;
    }
    unsigned i = (unsigned)state - 1;
    if (i == 0)
        wifi_ap_sort(); // sort once, immediately before rendering the list
    if (i >= wifi_ap_count)
    {
        if (i == 0)
            com_snprintf_utf8(buf, buf_size, "%s\n", S(STR_WIFI_NO_NETWORKS));
        return -1;
    }
    wifi_scan_format(i, buf, buf_size);
    return state + 1;
}

bool wifi_ready(void)
{
    return wifi_state == wifi_state_connected;
}

bool wifi_connecting(void)
{
    return wifi_state == wifi_state_connect ||
           wifi_state == wifi_state_connecting ||
           (wifi_state == wifi_state_connect_failed &&
            wifi_retry_count < WIFI_RETRY_INITIAL_RETRIES);
}

void wifi_load_ssid(const char *str)
{
    size_t n = strlen(str);
    if (n < sizeof(wifi_ssid))
    {
        memcpy(wifi_ssid, str, n);
        wifi_ssid[n] = 0;
    }
}

bool wifi_set_ssid(const char *ssid)
{
    size_t len = strlen(ssid);
    if (len < sizeof(wifi_ssid))
    {
        if (strcmp(wifi_ssid, ssid))
        {
            wifi_pass[0] = 0;
            strncpy(wifi_ssid, ssid, sizeof(wifi_ssid));
            wifi_shutdown();
        }
        cfg_save();
        return true;
    }
    return false;
}

const char *wifi_get_ssid(void)
{
    return wifi_ssid;
}

void wifi_load_pass(const char *str)
{
    size_t n = strlen(str);
    if (n < sizeof(wifi_pass))
    {
        memcpy(wifi_pass, str, n);
        wifi_pass[n] = 0;
    }
}

bool wifi_set_pass(const char *pass)
{
    if (strlen(wifi_ssid) && strlen(pass) < sizeof(wifi_pass))
    {
        if (strcmp(wifi_pass, pass))
        {
            strncpy(wifi_pass, pass, sizeof(wifi_pass));
            wifi_shutdown();
        }
        cfg_save();
        return true;
    }
    return false;
}

const char *wifi_get_pass(void)
{
    return wifi_pass;
}

#endif /* RP6502_RIA_W */
