/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_API_STD_H_
#define _EMU_API_STD_H_

#ifdef __cplusplus
extern "C"
{
#endif

#include "api/std.h"

void std_reset(void); /* machine reset: close open files + dirs, reset the console */

#ifdef __cplusplus
}
#endif

#endif /* _EMU_API_STD_H_ */
