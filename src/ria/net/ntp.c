/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "pico.h"

#ifndef RASPBERRYPI_PICO2_W
void ntp_task(void) {}
void ntp_print_status(void) {}
#else

#include "net/net.h"
#include "net/ntp.h"
#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include "pico/aon_timer.h"
#include <string.h>

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
    ntp_state_request_fail,
    ntp_state_set_time_fail,
    ntp_state_success,
    ntp_state_internal_error,
} ntp_state_t;
ntp_state_t ntp_state;

ip_addr_t ntp_server_address;
struct udp_pcb *ntp_pcb;

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
        ntp_state = ntp_state_dns_fail;
    }
}

static void ntp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    (void)arg;
    (void)pcb;
    uint8_t mode = pbuf_get_at(p, 0) & 0x7;
    uint8_t stratum = pbuf_get_at(p, 1);

    if (ip_addr_cmp(addr, &ntp_server_address) && port == NTP_PORT && p->tot_len == NTP_MSG_LEN &&
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
            ntp_state = ntp_state_success;
        else
            ntp_state = ntp_state_set_time_fail;
    }
    else
        ntp_state = ntp_state_request_fail;
    pbuf_free(p);
}

void ntp_task(void)
{
    if (ntp_state == ntp_state_internal_error)
        return;
    if (!net_ready())
    {
        ntp_state = ntp_state_init;
        return;
    }
    switch (ntp_state)
    {
    case ntp_state_init:
        ntp_pcb = udp_new_ip_type(IPADDR_TYPE_ANY);
        if (!ntp_pcb)
            ntp_state = ntp_state_internal_error;
        else
        {
            ntp_state = ntp_state_dns;
            udp_recv(ntp_pcb, ntp_recv, NULL);
        }
        break;
    case ntp_state_dns:
        err_t err = dns_gethostbyname(NTP_SERVER, &ntp_server_address, ntp_dns_found, NULL);
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
        ntp_state = ntp_state_request_wait;
        break;
    case ntp_state_dns_wait:
    case ntp_state_dns_fail:
    case ntp_state_request_wait:
    case ntp_state_request_fail:
    case ntp_state_set_time_fail:
    case ntp_state_success:
    case ntp_state_internal_error:
        break;
    }
}

void ntp_print_status(void)
{
    printf("NTP Status: ");
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
    case ntp_state_request_fail:
        puts("failed");
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

    printf("Time: ");
    struct timespec ts;
    if (!aon_timer_get_time(&ts))
    {
        puts("get time failure");
    }
    else
    {
        // TODO make time zone configurable
        setenv("TZ", "UTC0", 1);
        // setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);
        // setenv("TZ", "<GMT-3>+3", 1);
        tzset();

        char buf[100];
        struct tm tminfo;
        localtime_r(&ts.tv_sec, &tminfo);
        strftime(buf, sizeof(buf), "%c %z %Z", &tminfo);
        printf("%s\n", buf);
    }
}

#endif /* RASPBERRYPI_PICO2_W */
