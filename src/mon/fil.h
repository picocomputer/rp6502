
/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FIL_H_
#define _FIL_H_

#include <stddef.h>
#include <stdbool.h>

void fil_ls(const char *args, size_t len);
void fil_cd(const char *args, size_t len);
void fil_upload(const char *args, size_t len);
void fil_dispatch(const char *args, size_t len);
void fil_binary_handler();
bool fil_is_prompting();
bool fil_is_rx_binary();
void fil_task();
void fil_reset();

#endif /* _FIL_H_ */
