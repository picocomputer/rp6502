
/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FIL_H_
#define _FIL_H_

#include <stddef.h>
#include <stdbool.h>

void fil_chdir(const char *args, size_t len);
void fil_chdrive(const char *args, size_t len);
void fil_ls(const char *args, size_t len);
void fil_upload(const char *args, size_t len);
void fil_unlink(const char *args, size_t len);
void fil_command_dispatch(const char *args, size_t len);
void fil_task();
bool fil_is_prompting();
void fil_reset();

#endif /* _FIL_H_ */
