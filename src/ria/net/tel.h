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
bool tel_open(const char *hostname, uint16_t port);
err_t tel_close(bool force);

#endif /* _TEL_H_ */
