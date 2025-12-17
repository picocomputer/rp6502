/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_NET_NTP_H_
#define _RIA_NET_NTP_H_

/* Network Time Protocol.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void ntp_task(void);

/* Utility
 */

int ntp_status_response(char *buf, size_t buf_size, int state);

#endif /* _RIA_NET_NTP_H_ */
