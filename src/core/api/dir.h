/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_API_DIR_H_
#define _CORE_API_DIR_H_

/* The directory syscalls' side of the xstack: how a directory entry reaches the
 * 6502. The layout is FatFs's FILINFO because that is what the ABI was cut
 * from, so a drive that has no FatFs under it fills one in and pushes it here
 * rather than inventing a second spelling of the same struct. */

#include "fatfs/ff.h"
#include <stdbool.h>

/* Push one FILINFO onto the xstack in the 6502-visible field order. */
bool dir_push_filinfo(FILINFO *fno);

#endif /* _CORE_API_DIR_H_ */
