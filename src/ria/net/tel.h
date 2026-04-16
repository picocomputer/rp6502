/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_NET_TEL_H_
#define _RIA_NET_TEL_H_

/* Telnet protocol driver and console server.
 */

#include "net/net.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Protocol layer
 */

uint16_t tel_rx(int desc, char *buf, uint16_t len);
uint16_t tel_tx(int desc, const char *buf, uint16_t len);
bool tel_open(int desc, const char *hostname, uint16_t port,
              void (*on_close)(int));
void tel_close(int desc);
void tel_negotiate(int desc);
bool tel_listen(uint16_t port, net_accept_fn on_accept);
void tel_listen_close(uint16_t port);
bool tel_accept(int desc, uint16_t port, void (*on_close)(int));
void tel_reject(uint16_t port);
bool tel_has_pending(uint16_t port);

/* Console server
 */

void tel_init(void);
void tel_task(void);

bool tel_tx_writable(void);
void tel_tx_write(char ch);
void tel_pump(void);
void tel_flush(void);

int tel_read(char *buf, int length);

void tel_load_port(const char *str);
void tel_load_key(const char *str);
bool tel_set_port(uint32_t port);
bool tel_set_key(const char *key);
uint16_t tel_get_port(void);
const char *tel_get_key(void);

#endif /* _RIA_NET_TEL_H_ */
