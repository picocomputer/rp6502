
/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FIL_H_
#define _FIL_H_

#include <stddef.h>
#include <stdbool.h>

/* Kernel events
 */

void fil_task(void);
bool fil_active(void);
void fil_reset(void);

/* Monitor commands
 */

void fil_mon_chdir(const char *args, size_t len);
void fil_mon_mkdir(const char *args, size_t len);
void fil_mon_chdrive(const char *args, size_t len);
void fil_mon_ls(const char *args, size_t len);
void fil_mon_upload(const char *args, size_t len);
void fil_mon_unlink(const char *args, size_t len);

#endif /* _FIL_H_ */
