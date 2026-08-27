/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* FatFs, in this machine's vocabulary. Shared by everything here that holds a
 * FIL or a DIR of its own: api/fs.c, api/dir.c, mon/rom.c.
 */

#ifndef _RIA_API_FAT_H_
#define _RIA_API_FAT_H_

#include "core/api/api.h"
#include <stdint.h>

// Convert a FatFs FRESULT to a POSIX errno, and to an api_errno through it.
int fat_fresult_to_errno(unsigned fresult);
api_errno fat_fresult_to_api_errno(unsigned fresult);

// Set errno from a FRESULT, for the host/api/fs.h calls that report that way.
void fat_fail(unsigned fresult);

#endif /* _RIA_API_FAT_H_ */
