/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef RP6502_RIA_W

#include "net/mdm.h"
#include "net/net.h"
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
    net_state_closed,
    net_state_dns_lookup,
    net_state_connecting,
    net_state_connected,
    net_state_closing,
} net_state_t;

typedef struct
{
    net_state_t state;
    struct tcp_pcb *pcb;
    uint16_t port;
    struct pbuf *pbufs[PBUF_POOL_SIZE];
    uint8_t pbuf_head;
    uint8_t pbuf_tail;
    uint16_t pbuf_pos;
} net_conn_t;

static net_conn_t net_conns[MDM_MAX_CONNECTIONS];

static_assert(MDM_MAX_CONNECTIONS < MEMP_NUM_TCP_PCB);

static int net_desc(net_conn_t *nc)
{
    return (int)(nc - net_conns);
}

static void net_drain(net_conn_t *nc)
{
    while (nc->pbuf_head != nc->pbuf_tail)
    {
        pbuf_free(nc->pbufs[nc->pbuf_tail]);
        nc->pbuf_tail = (nc->pbuf_tail + 1) % PBUF_POOL_SIZE;
    }
    nc->pbuf_pos = 0;
}

void net_close(int desc)
{
    net_conn_t *nc = &net_conns[desc];
    net_state_t state = nc->state;
    nc->state = net_state_closed;
    if (state == net_state_connected || state == net_state_closing)
        net_drain(nc);
    if (state == net_state_closed)
        return;
    mdm_set_conn(desc);
    switch (state)
    {
    case net_state_dns_lookup:
    case net_state_connecting:
        mdm_dial_failed();
        break;
    case net_state_connected:
        mdm_carrier_lost();
        break;
    default:
        break;
    }
    if (nc->pcb)
        switch (state)
        {
        case net_state_connecting:
            DBG("NET tcp_abort\n");
            tcp_abort(nc->pcb);
            nc->pcb = NULL;
            return;
        case net_state_connected:
        case net_state_closing:
        {
            DBG("NET tcp_close\n");
            err_t err = tcp_close(nc->pcb);
            if (err != ERR_OK)
            {
                DBG("NET tcp_close failed\n");
                tcp_abort(nc->pcb);
            }
            nc->pcb = NULL;
            return;
        }
        case net_state_closed:
        case net_state_dns_lookup:
            break;
        }
}

uint16_t net_rx(int desc, char *buf, uint16_t len)
{
    net_conn_t *nc = &net_conns[desc];
    uint16_t total = 0;
    while (total < len && nc->pbuf_head != nc->pbuf_tail)
    {
        struct pbuf *p = nc->pbufs[nc->pbuf_tail];
        uint16_t avail = p->len - nc->pbuf_pos;
        uint16_t copy = (len - total < avail) ? (len - total) : avail;
        memcpy(buf + total, (char *)p->payload + nc->pbuf_pos, copy);
        total += copy;
        nc->pbuf_pos += copy;
        if (nc->pbuf_pos >= p->len)
        {
            if (p->next)
            {
                nc->pbufs[nc->pbuf_tail] = p->next;
                pbuf_ref(p->next);
            }
            else
            {
                nc->pbuf_tail = (nc->pbuf_tail + 1) % PBUF_POOL_SIZE;
            }
            pbuf_free(p);
            nc->pbuf_pos = 0;
        }
    }
    if (total && nc->pcb)
        tcp_recved(nc->pcb, total);
    if (nc->pbuf_head == nc->pbuf_tail && nc->state == net_state_closing)
        net_close(desc);
    return total;
}

uint16_t net_tx(int desc, const char *buf, uint16_t len)
{
    net_conn_t *nc = &net_conns[desc];
    if (!nc->pcb)
        return 0;
    if (nc->state == net_state_connected)
    {
        u16_t space = tcp_sndbuf(nc->pcb);
        if (space == 0)
            return 0;
        if (len > space)
            len = space;
        err_t err = tcp_write(nc->pcb, buf, len, TCP_WRITE_FLAG_COPY);
        if (err == ERR_OK)
        {
            tcp_output(nc->pcb);
            return len;
        }
        if (err == ERR_CONN)
            net_close(desc);
    }
    return 0;
}

static err_t net_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    net_conn_t *nc = (net_conn_t *)arg;
    int desc = net_desc(nc);
    (void)tpcb;
    (void)err;
    assert(err == ERR_OK);
    if (!p)
    {
        nc->state = net_state_closing;
        mdm_set_conn(desc);
        mdm_carrier_lost();
        if (nc->pbuf_head == nc->pbuf_tail)
            net_close(desc);
        return ERR_OK;
    }
    if (nc->state == net_state_connected)
    {
        uint8_t next = (nc->pbuf_head + 1) % PBUF_POOL_SIZE;
        if (next == nc->pbuf_tail)
            return ERR_MEM;
        nc->pbufs[nc->pbuf_head] = p;
        nc->pbuf_head = next;
        return ERR_OK;
    }
    pbuf_free(p);
    return ERR_OK;
}

static err_t net_connected(void *arg, struct tcp_pcb *tpcb, err_t err)
{
    net_conn_t *nc = (net_conn_t *)arg;
    (void)tpcb;
    (void)err;
    assert(err == ERR_OK);
    DBG("NET TCP Connected %d\n", err);
    nc->state = net_state_connected;
    mdm_set_conn(net_desc(nc));
    mdm_connect();
    return ERR_OK;
}

static void net_err(void *arg, err_t err)
{
    net_conn_t *nc = (net_conn_t *)arg;
    (void)err;
    DBG("NET tcp_err %d\n", err);
    nc->pcb = NULL; // PCB already freed by lwip
    net_close(net_desc(nc));
}

static void net_dns_found(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    net_conn_t *nc = (net_conn_t *)arg;
    (void)name;
    if (nc->state != net_state_dns_lookup)
        return;
    if (!ipaddr)
    {
        DBG("NET DNS did not resolve\n");
        net_close(net_desc(nc));
        return;
    }
    nc->pcb = tcp_new_ip_type(IP_GET_TYPE(ipaddr));
    if (!nc->pcb)
    {
        DBG("NET tcp_new_ip_type failed\n");
        net_close(net_desc(nc));
        return;
    }
    DBG("NET connecting\n");
    nc->state = net_state_connecting;
    tcp_arg(nc->pcb, nc);
    tcp_nagle_disable(nc->pcb);
    tcp_err(nc->pcb, net_err);
    tcp_recv(nc->pcb, net_recv);
    err_t err = tcp_connect(nc->pcb, ipaddr, nc->port, net_connected);
    if (err != ERR_OK)
    {
        DBG("NET tcp_connect failed %d\n", err);
        net_close(net_desc(nc));
    }
}

bool net_open(int desc, const char *hostname, uint16_t port)
{
    net_conn_t *nc = &net_conns[desc];
    assert(nc->state == net_state_closed);
    ip_addr_t ipaddr;
    nc->port = port;
    err_t err = dns_gethostbyname(hostname, &ipaddr, net_dns_found, nc);
    if (err == ERR_INPROGRESS)
    {
        DBG("NET DNS looking up\n");
        nc->state = net_state_dns_lookup;
        return true;
    }
    if (err == ERR_OK)
    {
        DBG("NET DNS resolved locally\n");
        nc->state = net_state_dns_lookup;
        net_dns_found(hostname, &ipaddr, nc);
        return nc->state == net_state_connecting;
    }
    DBG("NET dns_gethostbyname (%d)\n", err);
    return false;
}

#endif /* RP6502_RIA_W */
