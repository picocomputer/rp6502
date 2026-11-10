/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _OSAL_PICO_FS_H_
#define _OSAL_PICO_FS_H_

#include "fatfs/ff.h"

/* A file object of the 6502's descriptor pool that no descriptor holds, or
 * NULL. f_open takes it and f_close gives it back. The monitor uses these
 * while the 6502 is stopped. */
FIL *fs_fat_spare(void);

#endif /* _OSAL_PICO_FS_H_ */
