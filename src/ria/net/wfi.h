/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_NET_WFI_H_
#define _RIA_NET_WFI_H_

/* Wi-Fi driver.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void wfi_task(void);

/* Utility
 */

void wfi_print_status(void);
void wfi_shutdown(void);
bool wfi_ready(void);

#endif /* _RIA_NET_WFI_H_ */
