/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _NET_H_
#define _NET_H_

#include <stdbool.h>

void net_init(void);
void net_task(void);
void net_led(bool ison);

#endif /* _NET_H_ */
