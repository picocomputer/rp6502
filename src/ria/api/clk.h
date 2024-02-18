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

/* The API implementaiton for time support
 */

void clk_api_clock(void);
void clk_api_get_res(void);
void clk_api_get_time(void);
void clk_api_set_time(void);

#endif /* _CLK_H_ */
