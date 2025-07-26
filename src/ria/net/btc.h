/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _BTX_H_
#define _BTX_H_

#include <stdint.h>
#include <stdbool.h>

/* Kernel events
 */

void btc_task(void);

/* Utility
 */

void btc_set_config(uint8_t bt); // initiate Bluetooth gamepad pairing mode

void btc_shutdown(void); // called before cyw radio is turned off

void btc_print_status(void);

#endif /* _BTX_H_ */
