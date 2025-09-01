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

void ntp_print_status(void);

#endif /* _RIA_NET_NTP_H_ */
