/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_NET_TEL_H_
#define _RIA_NET_TEL_H_

/* Telnet driver for the modem.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Utility
 */

uint16_t tel_rx(char *buf, uint16_t len);
uint16_t tel_tx(const char *buf, uint16_t len);
bool tel_open(const char *hostname, uint16_t port);
void tel_close(void);

#endif /* _RIA_NET_TEL_H_ */
