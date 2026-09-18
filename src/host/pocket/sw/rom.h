/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_ROM_H_
#define _HOST_POCKET_SW_ROM_H_

#include <stdbool.h>

bool rom_load(const char *path);

bool rom_load_fd(int fd);

#endif /* _HOST_POCKET_SW_ROM_H_ */
