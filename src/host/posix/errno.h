/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* What went wrong, in the API's words. This host's failures arrive as POSIX
 * errno because POSIX is what it is, so the map is one hop and lives here --
 * beside the calls that produce it, not in core, which never sees an errno.
 */

#ifndef _HOST_POSIX_ERRNO_H_
#define _HOST_POSIX_ERRNO_H_

#include "core/api/api.h"

api_errno errno_to_api(int host_errno);

#endif /* _HOST_POSIX_ERRNO_H_ */
