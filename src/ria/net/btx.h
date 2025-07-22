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

void btx_task(void);
void btx_reset(void);

/* Utility
 */

void btx_start_pairing(void); // initiate Bluetooth gamepad pairing mode

void btx_disconnect(void); // called before cyw radio is turned off

void btx_print_status(void);

#endif /* _BTX_H_ */
