/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_APP_PNG_H_
#define _EMU_APP_PNG_H_

#include <stdbool.h>
#include <stdint.h>

bool png_write(const char *path, int w, int h, const uint32_t *rgba);

#endif /* _EMU_APP_PNG_H_ */
