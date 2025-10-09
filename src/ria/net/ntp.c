/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef RP6502_RIA_W
#include "net/ntp.h"
void ntp_task() {}
void ntp_print_status() {}
#else

#include "net/ntp.h"
#include "net/wfi.h"
#include <lwip/dns.h>
#include <lwip/udp.h>
#include <pico/aon_timer.h>
#include <pico/time.h>
#include <string.h>

#if defined(DEBUG_RIA_NET) || defined(DEBUG_RIA_NET_NTP)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define NTP_SERVER "pool.ntp.org"
#define NTP_MSG_LEN 48
#define NTP_PORT 123
#define NTP_DELTA 2208988800 // (1 Jan 1970) - (1 Jan 1900)

typedef enum
{
    ntp_state_init,
    ntp_state_dns,
    ntp_state_dns_wait,
    ntp_state_dns_fail,
    ntp_state_request,
    ntp_state_request_wait,
    ntp_state_request_timeout,
    ntp_state_set_time_fail,
    ntp_state_success,
    ntp_state_internal_error,
} ntp_state_t;
static ntp_state_t ntp_state;

static ip_addr_t ntp_server_address;
static struct udp_pcb *ntp_pcb;

static bool ntp_success_at_least_once;
static int ntp_retry_retry_count;
static absolute_time_t ntp_retry_timer;
static absolute_time_t ntp_timeout_timer;

// Be aggressive 5 times then back off
#define NTP_RETRY_RETRIES 5
#define NTP_RETRY_RETRY_SECS 2
#define NTP_RETRY_UNSET_SECS 60
#define NTP_RETRY_REFRESH_SECS (24 * 3600)
#define NTP_TIMEOUT_SECS 2

static void ntp_retry(void)
{
    if (ntp_retry_retry_count < NTP_RETRY_RETRIES)
    {
        ntp_retry_retry_count++;
        ntp_retry_timer = make_timeout_time_ms(NTP_RETRY_RETRY_SECS * 1000);
    }
    else
        ntp_retry_timer = make_timeout_time_ms(NTP_RETRY_UNSET_SECS * 1000);
}

static void ntp_dns_found(const char *hostname, const ip_addr_t *ipaddr, void *arg)
{
    (void)arg;
    (void)hostname;
    if (ipaddr)
    {
        ntp_server_address = *ipaddr;
        ntp_state = ntp_state_request;
    }
    else
    {
        DBG("NET NTP DNS fail\n");
        ntp_retry();
        ntp_state = ntp_state_dns_fail;
    }
}

static void ntp_udp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    (void)arg;
    (void)pcb;
    uint8_t mode = pbuf_get_at(p, 0) & 0x7;
    uint8_t stratum = pbuf_get_at(p, 1);

    if (ip_addr_cmp(addr, &ntp_server_address) &&
        port == NTP_PORT && p->tot_len == NTP_MSG_LEN &&
        mode == 0x4 && stratum != 0)
    {
        uint8_t seconds_buf[4] = {0};
        pbuf_copy_partial(p, seconds_buf, sizeof(seconds_buf), 40);
        uint32_t seconds_since_1900 = seconds_buf[0] << 24 | seconds_buf[1] << 16 | seconds_buf[2] << 8 | seconds_buf[3];
        uint32_t seconds_since_1970 = seconds_since_1900 - NTP_DELTA;
        time_t epoch = seconds_since_1970;
        struct timespec ts;
        ts.tv_sec = epoch;
        ts.tv_nsec = 0;
        if (aon_timer_set_time(&ts))
        {
            DBG("NET NTP success\n");
            ntp_success_at_least_once = true;
            ntp_retry_timer = make_timeout_time_ms(NTP_RETRY_REFRESH_SECS * 1000);
            ntp_state = ntp_state_success;
        }
        else
        {
            DBG("NET NTP set time fail\n");
            ntp_retry();
            ntp_state = ntp_state_set_time_fail;
        }
    }
    pbuf_free(p);
}

void ntp_task(void)
{
    if (!wfi_ready() && ntp_state != ntp_state_success)
    {
        ntp_state = ntp_state_init;
        return;
    }

    switch (ntp_state)
    {
    case ntp_state_init:
        DBG("NET NTP started\n");
        ntp_retry_retry_count = 0;
        if (!ntp_pcb)
            ntp_pcb = udp_new_ip_type(IPADDR_TYPE_ANY);
        if (!ntp_pcb)
            ntp_state = ntp_state_internal_error;
        else
        {
            ntp_state = ntp_state_dns;
            udp_recv(ntp_pcb, ntp_udp_recv, NULL);
        }
        break;
    case ntp_state_dns:
        err_t err = dns_gethostbyname(NTP_SERVER, &ntp_server_address, ntp_dns_found, NULL);
        ntp_timeout_timer = make_timeout_time_ms(NTP_TIMEOUT_SECS * 1000);
        if (err == ERR_OK)
            ntp_state = ntp_state_request;
        else
            ntp_state = ntp_state_dns_wait;
        break;
    case ntp_state_request:
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, NTP_MSG_LEN, PBUF_RAM);
        uint8_t *req = (uint8_t *)p->payload;
        memset(req, 0, NTP_MSG_LEN);
        req[0] = 0x1b;
        udp_sendto(ntp_pcb, p, &ntp_server_address, NTP_PORT);
        pbuf_free(p);
        ntp_timeout_timer = make_timeout_time_ms(NTP_TIMEOUT_SECS * 1000);
        ntp_state = ntp_state_request_wait;
        break;
    case ntp_state_request_wait:
    case ntp_state_dns_wait:
        if (absolute_time_diff_us(get_absolute_time(), ntp_timeout_timer) < 0)
        {
            DBG("NET NTP request timeout\n");
            ntp_retry();
            ntp_state = ntp_state_request_timeout;
        }
        break;
    case ntp_state_internal_error:
        break;
    case ntp_state_success:
    case ntp_state_dns_fail:
    case ntp_state_request_timeout:
    case ntp_state_set_time_fail:
        if (absolute_time_diff_us(get_absolute_time(), ntp_retry_timer) < 0)
        {
            ntp_state = ntp_state_init;
            break;
        }
    }
}

void ntp_print_status(void)
{
    printf("NTP : ");
    switch (ntp_state)
    {
    case ntp_state_init:
        puts("no network");
        break;
    case ntp_state_dns:
    case ntp_state_dns_wait:
        puts("DNS lookup");
        break;
    case ntp_state_dns_fail:
        puts("DNS fail");
        break;
    case ntp_state_request:
    case ntp_state_request_wait:
        puts("requested");
        break;
    case ntp_state_request_timeout:
        puts("request timeout");
        break;
    case ntp_state_set_time_fail:
        puts("set time failure");
        break;
    case ntp_state_success:
        puts("success");
        break;
    case ntp_state_internal_error:
        puts("internal error");
        break;
    }
}

#endif /* RP6502_RIA_W */
