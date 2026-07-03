/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_PNG_H_
#define _EMU_PNG_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

bool emu_write_png(const char *path, int w, int h, const uint32_t *rgba);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_PNG_H_ */
