/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* What went wrong, in the API's words. This machine has two filesystems and
 * so two vocabularies -- FatFs on the drive, littlefs in flash -- and both
 * map here rather than in core, which sees neither.
 *
 * Named errmap and not errno because a host directory is on the include path
 * (this machine puts one there so host.h can be reached), and a header
 * called errno.h there is the one every C library picks up instead of its
 * own. Renaming it back breaks every translation unit at once.
 */

#ifndef _OSAL_PICO_ERRMAP_H_
#define _OSAL_PICO_ERRMAP_H_

#include "core/api/api.h"

api_errno fresult_to_api(unsigned fresult);
api_errno lfs_error_to_api(int lfs_err);

#endif /* _OSAL_PICO_ERRMAP_H_ */
