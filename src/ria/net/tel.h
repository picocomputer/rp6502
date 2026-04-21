/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_NET_TEL_H_
#define _RIA_NET_TEL_H_

/* Telnet protocol driver.
 */

#include "net/net.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

uint16_t tel_rx(int desc, char *buf, uint16_t len);
uint16_t tel_tx(int desc, const char *buf, uint16_t len);
bool tel_open(int desc, const char *hostname, uint16_t port,
              void (*on_close)(int));
void tel_close(int desc);
void tel_negotiate(int desc);
bool tel_listen(uint16_t port, net_accept_fn on_accept);
void tel_listen_close(uint16_t port);
bool tel_accept(int desc, uint16_t port, void (*on_close)(int));
bool tel_accept_server(int desc, uint16_t port, void (*on_close)(int));
void tel_reject(uint16_t port);
bool tel_has_pending(uint16_t port);

#endif /* _RIA_NET_TEL_H_ */
