/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_NET_NET_H_
#define _RIA_NET_NET_H_

/* Network transport driver for the modem.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

uint16_t net_rx(int desc, char *buf, uint16_t len);
uint16_t net_tx(int desc, const char *buf, uint16_t len);
bool net_open(int desc, const char *hostname, uint16_t port);
void net_close(int desc);
bool net_listen(int desc, uint16_t port);
void net_listen_close(int desc, uint16_t port);
bool net_accept(int desc, uint16_t port);
void net_reject(uint16_t port);
bool net_has_pending(uint16_t port);

#endif /* _RIA_NET_NET_H_ */
