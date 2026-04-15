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

typedef struct
{
    tel_state_t state;
    struct tcp_pcb *pcb;
    uint16_t port;
    struct pbuf *pbufs[PBUF_POOL_SIZE];
    uint8_t pbuf_head;
    uint8_t pbuf_tail;
    uint16_t pbuf_pos;
} tel_conn_t;

static tel_conn_t tel_conns[MDM_MAX_CONNECTIONS];

static_assert(MDM_MAX_CONNECTIONS < MEMP_NUM_TCP_PCB);

static int tel_desc(tel_conn_t *tc)
{
    return (int)(tc - tel_conns);
}

static void tel_drain(tel_conn_t *tc)
{
    while (tc->pbuf_head != tc->pbuf_tail)
    {
        pbuf_free(tc->pbufs[tc->pbuf_tail]);
        tc->pbuf_tail = (tc->pbuf_tail + 1) % PBUF_POOL_SIZE;
    }
    tc->pbuf_pos = 0;
}

void tel_close(int desc)
{
    tel_conn_t *tc = &tel_conns[desc];
    tel_state_t state = tc->state;
    tc->state = tel_state_closed;
    if (state == tel_state_connected || state == tel_state_closing)
        tel_drain(tc);
    if (state == tel_state_closed)
        return;
    mdm_set_conn(desc);
    switch (state)
    {
    case tel_state_dns_lookup:
    case tel_state_connecting:
        mdm_dial_failed();
        break;
    case tel_state_connected:
        mdm_carrier_lost();
        break;
    default:
        break;
    }
    if (tc->pcb)
        switch (state)
        {
        case tel_state_connecting:
            DBG("NET TEL tcp_abort\n");
            tcp_abort(tc->pcb);
            tc->pcb = NULL;
            return;
        case tel_state_connected:
        case tel_state_closing:
        {
            DBG("NET TEL tcp_close\n");
            err_t err = tcp_close(tc->pcb);
            if (err != ERR_OK)
            {
                DBG("NET TEL tcp_close failed\n");
                tcp_abort(tc->pcb);
            }
            tc->pcb = NULL;
            return;
        }
        case tel_state_closed:
        case tel_state_dns_lookup:
            break;
        }
}

uint16_t tel_rx(int desc, char *buf, uint16_t len)
{
    tel_conn_t *tc = &tel_conns[desc];
    uint16_t total = 0;
    while (total < len && tc->pbuf_head != tc->pbuf_tail)
    {
        struct pbuf *p = tc->pbufs[tc->pbuf_tail];
        uint16_t avail = p->len - tc->pbuf_pos;
        uint16_t copy = (len - total < avail) ? (len - total) : avail;
        memcpy(buf + total, (char *)p->payload + tc->pbuf_pos, copy);
        total += copy;
        tc->pbuf_pos += copy;
        if (tc->pbuf_pos >= p->len)
        {
            if (p->next)
            {
                tc->pbufs[tc->pbuf_tail] = p->next;
                pbuf_ref(p->next);
            }
            else
            {
                tc->pbuf_tail = (tc->pbuf_tail + 1) % PBUF_POOL_SIZE;
            }
            pbuf_free(p);
            tc->pbuf_pos = 0;
        }
    }
    if (total && tc->pcb)
        tcp_recved(tc->pcb, total);
    if (tc->pbuf_head == tc->pbuf_tail && tc->state == tel_state_closing)
        tel_close(desc);
    return total;
}

uint16_t tel_tx(int desc, const char *buf, uint16_t len)
{
    tel_conn_t *tc = &tel_conns[desc];
    if (!tc->pcb)
        return 0;
    if (tc->state == tel_state_connected)
    {
        u16_t space = tcp_sndbuf(tc->pcb);
        if (space == 0)
            return 0;
        if (len > space)
            len = space;
        err_t err = tcp_write(tc->pcb, buf, len, TCP_WRITE_FLAG_COPY);
        if (err == ERR_OK)
        {
            tcp_output(tc->pcb);
            return len;
        }
        if (err == ERR_CONN)
            tel_close(desc);
    }
    return 0;
}

static err_t tel_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    tel_conn_t *tc = (tel_conn_t *)arg;
    int desc = tel_desc(tc);
    (void)tpcb;
    (void)err;
    assert(err == ERR_OK);
    if (!p)
    {
        tc->state = tel_state_closing;
        mdm_set_conn(desc);
        mdm_carrier_lost();
        if (tc->pbuf_head == tc->pbuf_tail)
            tel_close(desc);
        return ERR_OK;
    }
    if (tc->state == tel_state_connected)
    {
        uint8_t next = (tc->pbuf_head + 1) % PBUF_POOL_SIZE;
        if (next == tc->pbuf_tail)
            return ERR_MEM;
        tc->pbufs[tc->pbuf_head] = p;
        tc->pbuf_head = next;
        return ERR_OK;
    }
    pbuf_free(p);
    return ERR_OK;
}

static err_t tel_connected(void *arg, struct tcp_pcb *tpcb, err_t err)
{
    tel_conn_t *tc = (tel_conn_t *)arg;
    (void)tpcb;
    (void)err;
    assert(err == ERR_OK);
    DBG("NET TEL TCP Connected %d\n", err);
    tc->state = tel_state_connected;
    mdm_set_conn(tel_desc(tc));
    mdm_connect();
    return ERR_OK;
}

static void tel_err(void *arg, err_t err)
{
    tel_conn_t *tc = (tel_conn_t *)arg;
    (void)err;
    DBG("NET TEL tcp_err %d\n", err);
    tc->pcb = NULL; // PCB already freed by lwip
    tel_close(tel_desc(tc));
}

static void tel_dns_found(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    tel_conn_t *tc = (tel_conn_t *)arg;
    (void)name;
    if (tc->state != tel_state_dns_lookup)
        return;
    if (!ipaddr)
    {
        DBG("NET TEL DNS did not resolve\n");
        tel_close(tel_desc(tc));
        return;
    }
    tc->pcb = tcp_new_ip_type(IP_GET_TYPE(ipaddr));
    if (!tc->pcb)
    {
        DBG("NET TEL tcp_new_ip_type failed\n");
        tel_close(tel_desc(tc));
        return;
    }
    DBG("NET TEL connecting\n");
    tc->state = tel_state_connecting;
    tcp_arg(tc->pcb, tc);
    tcp_nagle_disable(tc->pcb);
    tcp_err(tc->pcb, tel_err);
    tcp_recv(tc->pcb, tel_recv);
    err_t err = tcp_connect(tc->pcb, ipaddr, tc->port, tel_connected);
    if (err != ERR_OK)
    {
        DBG("NET TEL tcp_connect failed %d\n", err);
        tel_close(tel_desc(tc));
    }
}

bool tel_open(int desc, const char *hostname, uint16_t port)
{
    tel_conn_t *tc = &tel_conns[desc];
    assert(tc->state == tel_state_closed);
    ip_addr_t ipaddr;
    tc->port = port;
    err_t err = dns_gethostbyname(hostname, &ipaddr, tel_dns_found, tc);
    if (err == ERR_INPROGRESS)
    {
        DBG("NET TEL DNS looking up\n");
        tc->state = tel_state_dns_lookup;
        return true;
    }
    if (err == ERR_OK)
    {
        DBG("NET TEL DNS resolved locally\n");
        tc->state = tel_state_dns_lookup;
        tel_dns_found(hostname, &ipaddr, tc);
        return tc->state == tel_state_connecting;
    }
    DBG("NET TEL dns_gethostbyname (%d)\n", err);
    return false;
}

#endif /* RP6502_RIA_W */
