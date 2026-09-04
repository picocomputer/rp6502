/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* What went wrong, in the API's words. This host's failures arrive as POSIX
 * errno because POSIX is what it is, so the map is one hop and lives here --
 * beside the calls that produce it, not in core, which never sees an errno.
 *
 * Named errmap and not errno: this maps one vocabulary onto another, which is
 * not what a C library's errno.h is, and a header of that name anywhere a
 * build might put on an include path is the one a translation unit picks up
 * instead of its own.
 */

#ifndef _OSAL_POSIX_ERRMAP_H_
#define _OSAL_POSIX_ERRMAP_H_

#include "core/api/api.h"

api_errno errno_to_api(int host_errno);

#endif /* _OSAL_POSIX_ERRMAP_H_ */
