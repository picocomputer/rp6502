/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _WFI_H_
#define _WFI_H_

#include <stdint.h>
#include <stdbool.h>

/* Kernel events
 */

void wfi_task(void);

/* Utility
 */

void wfi_print_status(void);
void wfi_disconnect(void);
bool wfi_ready(void);

#endif /* _WFI_H_ */
