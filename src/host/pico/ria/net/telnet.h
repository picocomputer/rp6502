/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_NET_TELNET_H_
#define _RIA_NET_TELNET_H_

/* Telnet protocol driver.
 */

#include "ria/net/net.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

uint16_t telnet_rx(int desc, char *buf, uint16_t len);
uint16_t telnet_tx(int desc, const char *buf, uint16_t len);
bool telnet_get_naws(int desc, uint16_t *w, uint16_t *h);
bool telnet_open(int desc, const char *hostname, uint16_t port,
              void (*on_close)(int));
void telnet_close(int desc);
void telnet_negotiate(int desc, bool telnet_mode, const char *ttype);
bool telnet_listen(uint16_t port, net_accept_fn on_accept);
void telnet_listen_close(uint16_t port);
bool telnet_accept(int desc, uint16_t port, bool telnet_mode, const char *ttype,
                void (*on_close)(int));
bool telnet_accept_server(int desc, uint16_t port, void (*on_close)(int));
void telnet_reject(uint16_t port);
bool telnet_has_pending(uint16_t port);

#endif /* _RIA_NET_TELNET_H_ */
