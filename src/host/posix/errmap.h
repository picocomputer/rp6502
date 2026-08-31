/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* What went wrong, in the API's words. This host's failures arrive as POSIX
 * errno because POSIX is what it is, so the map is one hop and lives here --
 * beside the calls that produce it, not in core, which never sees an errno.
 *
 * Named errmap and not errno because a host directory is on the include path
 * (mach/emu.cmake puts it there so host.h can be reached), and a header
 * called errno.h there is the one every C library picks up instead of its
 * own. Renaming it back breaks every translation unit at once.
 */

#ifndef _HOST_POSIX_ERRMAP_H_
#define _HOST_POSIX_ERRMAP_H_

#include "core/api/api.h"

api_errno errno_to_api(int host_errno);

#endif /* _HOST_POSIX_ERRMAP_H_ */
