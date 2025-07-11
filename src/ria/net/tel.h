/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TEL_H_
#define _TEL_H_

#include "lwip/err.h"

/* Utility
 */

int tel_rx(char *ch);
bool tel_tx(char *ch, u16_t len);
bool tel_open(const char *hostname, u16_t port);
err_t tel_close(void);

#endif /* _TEL_H_ */
