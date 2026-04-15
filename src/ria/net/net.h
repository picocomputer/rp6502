/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_NET_NET_H_
#define _RIA_NET_NET_H_

/* Network transport layer.
 */

#include "net/mdm.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define NET_MAX_CONNECTIONS (MDM_MAX_CONNECTIONS + 1)
#define NET_MAX_LISTENERS (MDM_MAX_CONNECTIONS + 1)
#define SYS_TEL_DESC MDM_MAX_CONNECTIONS

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
