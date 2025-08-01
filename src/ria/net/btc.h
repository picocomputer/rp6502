/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _BTC_H_
#define _BTC_H_

#include <stdint.h>
#include <stdbool.h>

/* Kernel events
 */

void btc_task(void);

/* Utility
 */

void btc_set_config(uint8_t bt);

void btc_shutdown(void);

void btc_print_status(void);

#endif /* _BTC_H_ */
