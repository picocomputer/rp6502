/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "pico.h"

#ifndef RP6502_RIA_W
void tel_task() {}
#else

#define DEBUG_RIA_NET_TEL ////////////////////

#if defined(DEBUG_RIA_NET) || defined(DEBUG_RIA_NET_TEL)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__);
#else
#define DBG(...)
#endif

#include "net/tel.h"
#include "lwip/err.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwipopts.h"

typedef enum
{
    tel_state_off,
    tel_state_dns_lookup,
    tel_state_connecting,
    tel_state_connected_tcp,
    tel_state_connected_telnet,
} tel_state_t;
static tel_state_t tel_state;

static struct tcp_pcb tel_pcb;
static u16_t tel_port;
static ip_addr_t tel_ipaddr;

struct pbuf *tel_pbufs[PBUF_POOL_SIZE];
static uint8_t tel_pbuf_head;
static uint8_t tel_pbuf_tail;
static u16_t tel_pbuf_pos;

void tel_task()
{
}

static err_t tel_close(void)
{
    // do we need to free pbufs?
    tel_pbuf_pos = tel_pbuf_head = tel_pbuf_tail = 0;
    err_t err = ERR_OK;
    if (tel_state == tel_state_off || tel_state == tel_state_dns_lookup)
        return err;
    tcp_err(&tel_pcb, NULL);
    tcp_recv(&tel_pcb, NULL);
    tcp_sent(&tel_pcb, NULL);
    tcp_poll(&tel_pcb, NULL, 0);
    tcp_arg(&tel_pcb, NULL);
    err = tcp_close(&tel_pcb);
    if (err != ERR_OK)
    {
        tcp_abort(&tel_pcb);
        err = ERR_ABRT;
    }
    return err;
}

int tel_rx(char *ch)
{
    if (tel_pbuf_head == tel_pbuf_tail)
        return 0;
    struct pbuf *p = tel_pbufs[tel_pbuf_tail];
    *ch = ((char *)p->payload)[tel_pbuf_pos];
    if (++tel_pbuf_pos >= p->tot_len)
    {
        pbuf_free(tel_pbufs[tel_pbuf_tail++]);
        tel_pbuf_pos = 0;
    }
    return 1;
}

err_t tel_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    (void)arg;
    (void)tpcb;
    assert(err == ERR_OK);
    if (!p)
        return tel_close();
    tel_pbufs[tel_pbuf_head++] = p;
    return err;
}

static err_t tel_connected(void *arg, struct tcp_pcb *tpcb, err_t err)
{
    (void)arg;
    (void)tpcb;
    assert(err == ERR_OK); // current version of library always sends ok
    DBG("NET TEL TCP Connected %d\n", err);
    tel_state = tel_state_connected_tcp; /////// maybe telnet

    tcp_recv(&tel_pcb, tel_recv);
    // tcp_sent(&tel_pcb, tel_sent);
    // tcp_poll(&tel_pcb, tel_poll, 2);
    tcp_nagle_disable(&tel_pcb);

    return ERR_OK;
}

void tel_err(void *arg, err_t err)
{
    (void)arg;
    DBG("NET TEL tcp_err %d\n", err);
    tel_state = tel_state_off;
}

void tel_dns_found(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    (void)name;
    (void)ipaddr;
    (void)arg;
    if (tel_state != tel_state_dns_lookup)
        return;
    tel_pbuf_pos = tel_pbuf_head = tel_pbuf_tail = 0;
    tcp_arg(&tel_pcb, NULL);
    tcp_err(&tel_pcb, tel_err);
    err_t err = tcp_connect(&tel_pcb, &tel_ipaddr, tel_port, tel_connected);
    if (err != ERR_OK)
    {
        DBG("NET TEL tcp_connect failed %d\n", err);
        tel_state = tel_state_off;
        return;
    }
    DBG("NET TEL connecting\n");
    tel_state = tel_state_connecting;
}

bool tel_open(const char *hostname, uint16_t port)
{
    assert(tel_state == tel_state_off);
    tel_port = port;
    err_t err = dns_gethostbyname(hostname, &tel_ipaddr, tel_dns_found, NULL);
    if (err == ERR_INPROGRESS)
    {
        DBG("NET TEL DNS looking up\n");
        tel_state = tel_state_dns_lookup;
        return true;
    }
    if (err == ERR_OK)
    {
        DBG("NET TEL DNS in cache\n");
        tel_state = tel_state_dns_lookup;
        tel_dns_found(hostname, &tel_ipaddr, NULL);
        return true;
    }
    return false;
}

#endif /* RP6502_RIA_W */
