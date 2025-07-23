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

bool btx_start_pairing(void); // initiate Bluetooth gamepad pairing mode

void btx_disconnect_all(void); // called before cyw radio is turned off

void btx_print_status(void);

void btx_toggle_ssp(void); // toggle Secure Simple Pairing mode for compatibility testing

#endif /* _BTX_H_ */
