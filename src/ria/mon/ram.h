/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RAM_H_
#define _RAM_H_

#include <stddef.h>

void ram_binary(const char *args, size_t len);
void ram_address(const char *args, size_t len);
void ram_task();
bool ram_active();
void ram_reset();

#endif /* _RAM_H_ */
