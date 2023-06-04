/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _SYS_H_
#define _SYS_H_

#include <stddef.h>

void sys_reboot(const char *args, size_t len);
void sys_run_6502(const char *args, size_t len);

#endif /* _SYS_H_ */
