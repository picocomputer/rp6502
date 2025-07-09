/*
 * Copyright (c) 2023 Brentward
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CLK_H_
#define _CLK_H_

/* Kernel events
 */

void clk_init(void);
void clk_run(void);
void clk_print_status(void);
const char *clk_set_time_zone(const char *tz);

/* The API implementaiton for time support
 */

bool clk_api_clock(void);
bool clk_api_get_res(void);
bool clk_api_get_time(void);
bool clk_api_set_time(void);
bool clk_api_get_time_zone(void);

#endif /* _CLK_H_ */
