/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_NET_TEL_H_
#define _RIA_NET_TEL_H_

/* Telnet protocol driver for the modem.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

uint16_t tel_rx(int desc, char *buf, uint16_t len);
uint16_t tel_tx(int desc, const char *buf, uint16_t len);
bool tel_open(int desc, const char *hostname, uint16_t port);
void tel_close(int desc);
void tel_on_connect(int desc);

#endif /* _RIA_NET_TEL_H_ */
