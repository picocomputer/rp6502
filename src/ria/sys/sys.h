/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _SYS_H_
#define _SYS_H_

#include <stddef.h>

/* Monitor commands
 */

void sys_mon_reboot(const char *args, size_t len);
void sys_mon_reset(const char *args, size_t len);
void sys_mon_status(const char *args, size_t len);
void sys_init(void);

#endif /* _SYS_H_ */
