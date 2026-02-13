/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef RP6502_RIA_W

#include "net/mdm.h"
#include "net/tel.h"
#include <lwip/tcp.h>
#include <lwip/dns.h>
#include <string.h>

#if defined(DEBUG_RIA_NET) || defined(DEBUG_RIA_NET_TEL)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

typedef enum
{
    tel_state_closed,
    tel_state_dns_lookup,
    tel_state_connecting,
    tel_state_connected,
    tel_state_closing,
} tel_state_t;
static tel_state_t tel_state;

static struct tcp_pcb *tel_pcb;
static u16_t tel_port;

static struct pbuf *tel_pbufs[PBUF_POOL_SIZE];
static uint8_t tel_pbuf_head;
static uint8_t tel_pbuf_tail;
static u16_t tel_pbuf_pos;

static void tel_drain(void)
{
    while (tel_pbuf_head != tel_pbuf_tail)
    {
        pbuf_free(tel_pbufs[tel_pbuf_tail]);
        tel_pbuf_tail = (tel_pbuf_tail + 1) % PBUF_POOL_SIZE;
    }
    tel_pbuf_pos = 0;
}

void tel_close(void)
{
    tel_state_t state = tel_state;
    tel_state = tel_state_closed;
    if (state == tel_state_connected || state == tel_state_closing)
        tel_drain();
    if (state == tel_state_closed)
        return;
    mdm_hangup();
    if (tel_pcb)
        switch (state)
        {
        case tel_state_connecting:
            DBG("NET TEL tcp_abort\n");
            tcp_abort(tel_pcb);
            tel_pcb = NULL;
            return;
        case tel_state_connected:
        case tel_state_closing:
        {
            DBG("NET TEL tcp_close\n");
            err_t err = tcp_close(tel_pcb);
            if (err != ERR_OK)
            {
                DBG("NET TEL tcp_close failed\n");
                tcp_abort(tel_pcb);
            }
            tel_pcb = NULL;
            return;
        }
        case tel_state_closed:
        case tel_state_dns_lookup:
            break;
        }
}

uint16_t tel_rx(char *buf, uint16_t len)
{
    uint16_t total = 0;
    while (total < len && tel_pbuf_head != tel_pbuf_tail)
    {
        struct pbuf *p = tel_pbufs[tel_pbuf_tail];
        uint16_t avail = p->len - tel_pbuf_pos;
        uint16_t copy = (len - total < avail) ? (len - total) : avail;
        memcpy(buf + total, (char *)p->payload + tel_pbuf_pos, copy);
        total += copy;
        tel_pbuf_pos += copy;
        if (tel_pbuf_pos >= p->len)
        {
            if (p->next)
            {
                tel_pbufs[tel_pbuf_tail] = p->next;
                pbuf_ref(p->next);
            }
            else
            {
                tel_pbuf_tail = (tel_pbuf_tail + 1) % PBUF_POOL_SIZE;
            }
            pbuf_free(p);
            tel_pbuf_pos = 0;
        }
    }
    if (total && tel_pcb)
        tcp_recved(tel_pcb, total);
    if (tel_pbuf_head == tel_pbuf_tail && tel_state == tel_state_closing)
        tel_close();
    return total;
}

uint16_t tel_tx(const char *buf, uint16_t len)
{
    if (!tel_pcb)
        return 0;
    if (tel_state == tel_state_connected)
    {
        u16_t space = tcp_sndbuf(tel_pcb);
        if (space == 0)
            return 0;
        if (len > space)
            len = space;
        err_t err = tcp_write(tel_pcb, buf, len, TCP_WRITE_FLAG_COPY);
        if (err == ERR_OK)
        {
            tcp_output(tel_pcb);
            return len;
        }
        if (err == ERR_CONN)
            tel_close();
    }
    return 0;
}

static err_t tel_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    (void)arg;
    (void)tpcb;
    (void)err;
    assert(err == ERR_OK);
    if (!p)
    {
        tel_state = tel_state_closing;
        mdm_carrier_lost();
        if (tel_pbuf_head == tel_pbuf_tail)
            tel_close();
        return ERR_OK;
    }
    if (tel_state == tel_state_connected)
    {
        uint8_t next = (tel_pbuf_head + 1) % PBUF_POOL_SIZE;
        if (next == tel_pbuf_tail)
            return ERR_MEM;
        tel_pbufs[tel_pbuf_head] = p;
        tel_pbuf_head = next;
        return ERR_OK;
    }
    pbuf_free(p);
    return ERR_OK;
}

static err_t tel_connected(void *arg, struct tcp_pcb *tpcb, err_t err)
{
    (void)arg;
    (void)tpcb;
    (void)err;
    assert(err == ERR_OK);
    DBG("NET TEL TCP Connected %d\n", err);
    tel_state = tel_state_connected;
    mdm_connect();
    return ERR_OK;
}

static void tel_err(void *arg, err_t err)
{
    (void)arg;
    (void)err;
    DBG("NET TEL tcp_err %d\n", err);
    tel_pcb = NULL; // PCB already freed by lwip
    tel_close();
}

static void tel_dns_found(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    (void)name;
    (void)arg;
    if (tel_state != tel_state_dns_lookup)
        return;
    if (!ipaddr)
    {
        DBG("NET TEL DNS did not resolve\n");
        tel_close();
        return;
    }
    tel_pcb = tcp_new_ip_type(IP_GET_TYPE(ipaddr));
    if (!tel_pcb)
    {
        DBG("NET TEL tcp_new_ip_type failed\n");
        tel_close();
        return;
    }
    DBG("NET TEL connecting\n");
    tel_state = tel_state_connecting;
    tcp_nagle_disable(tel_pcb);
    tcp_err(tel_pcb, tel_err);
    tcp_recv(tel_pcb, tel_recv);
    err_t err = tcp_connect(tel_pcb, ipaddr, tel_port, tel_connected);
    if (err != ERR_OK)
    {
        DBG("NET TEL tcp_connect failed %d\n", err);
        tel_close();
    }
}

bool tel_open(const char *hostname, uint16_t port)
{
    assert(tel_state == tel_state_closed);
    ip_addr_t ipaddr;
    tel_port = port;
    err_t err = dns_gethostbyname(hostname, &ipaddr, tel_dns_found, NULL);
    if (err == ERR_INPROGRESS)
    {
        DBG("NET TEL DNS looking up\n");
        tel_state = tel_state_dns_lookup;
        return true;
    }
    if (err == ERR_OK)
    {
        DBG("NET TEL DNS resolved locally\n");
        tel_state = tel_state_dns_lookup;
        tel_dns_found(hostname, &ipaddr, NULL);
        return tel_state == tel_state_connecting;
    }
    DBG("NET TEL dns_gethostbyname (%d)\n", err);
    return false;
}

#endif /* RP6502_RIA_W */
