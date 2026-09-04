/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_W_NET_NTP_H_
#define _RIA_W_NET_NTP_H_

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

int ntp_status_response(char *buf, size_t buf_size, int state, unsigned width);

/* This driver's row in a machine's driver list; see core/sys/driver.h. Sets the clock once the network is up, which it checks for itself each
 * pass rather than being sequenced behind wifi. */
#define NTP_DRIVER DRIVER(nul_init, ntp_task, nul_task, nul_run, nul_stop, nul_break, nul_config, nul_config)

#endif /* _RIA_W_NET_NTP_H_ */
