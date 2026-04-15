/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_NET_NET_H_
#define _RIA_NET_NET_H_

/* Network transport layer.
 */

#include "lwipopts.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define NET_REM_DESCS 1
#define NET_MAX_CONNECTIONS MEMP_NUM_TCP_PCB_LISTEN
#define NET_MAX_LISTENERS MEMP_NUM_TCP_PCB_LISTEN
#define NET_MDM_DESCS (NET_MAX_CONNECTIONS - NET_REM_DESCS)
#define SYS_TEL_DESC NET_MDM_DESCS
#define NET_CONN_PBUF_DEPTH (TCP_WND / TCP_MSS + 1)

typedef bool (*net_accept_fn)(uint16_t port);

uint16_t net_rx(int desc, char *buf, uint16_t len);
uint16_t net_tx(int desc, const char *buf, uint16_t len);
bool net_open(int desc, const char *hostname, uint16_t port,
              void (*on_close)(int));
void net_close(int desc);
bool net_listen(uint16_t port, net_accept_fn on_accept);
void net_listen_close(uint16_t port);
bool net_accept(int desc, uint16_t port, void (*on_close)(int));
void net_reject(uint16_t port);
bool net_has_pending(uint16_t port);
bool net_is_closed(int desc);

#endif /* _RIA_NET_NET_H_ */
