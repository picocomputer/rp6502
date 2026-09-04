/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* What went wrong, in the API's words. This machine has two filesystems and
 * so two vocabularies -- FatFs on the drive, littlefs in flash -- and both
 * map here rather than in core, which sees neither.
 *
 * Named errmap and not errno: this maps one vocabulary onto another, which is
 * not what a C library's errno.h is, and a header of that name anywhere a
 * build might put on an include path is the one a translation unit picks up
 * instead of its own.
 */

#ifndef _OSAL_PICO_ERRMAP_H_
#define _OSAL_PICO_ERRMAP_H_

#include "core/api/api.h"

api_errno fresult_to_api(unsigned fresult);
api_errno lfs_error_to_api(int lfs_err);

#endif /* _OSAL_PICO_ERRMAP_H_ */
