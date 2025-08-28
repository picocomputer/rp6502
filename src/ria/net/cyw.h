/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_NET_CYW_H_
#define _RIA_NET_CYW_H_

/* Device driver for the CYW43 radio module.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Kernel events
 */

void cyw_task(void);
void cyw_pre_reclock(void);
void cyw_post_reclock(uint32_t sys_clk_khz);

/* Utility
 */

void cyw_led(bool ison);
bool cyw_validate_country_code(char *cc);
void cyw_reset_radio(void);

#endif /* _RIA_NET_CYW_H_ */
