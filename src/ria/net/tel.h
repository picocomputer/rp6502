/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TEL_H_
#define _TEL_H_

/* Kernel events
 */

void tel_task(void);
bool tel_open(const char *hostname, uint16_t port);

#endif /* _TEL_H_ */
