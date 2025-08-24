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
#include "lwip/err.h"

/* Utility
 */

int tel_rx(char *ch);
bool tel_tx(char *ch, u16_t len);
bool tel_open(const char *hostname, u16_t port);
err_t tel_close(void);

#endif /* _RIA_NET_TEL_H_ */
