
/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _ROM_H_
#define _ROM_H_

#include <stddef.h>
#include <stdbool.h>

void rom_load_fat(const char *args, size_t len);
bool rom_load_lfs(const char *args, size_t len);
void rom_help_fat(const char *args, size_t len);
bool rom_help_lfs(const char *args, size_t len);
void rom_install(const char *args, size_t len);
void rom_remove(const char *args, size_t len);
void rom_task();
bool rom_is_active();
void rom_reset();

#endif /* _ROM_H_ */
