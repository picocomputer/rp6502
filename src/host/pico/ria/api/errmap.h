/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* What went wrong, in the API's words. This machine's failures arrive as a
 * FatFs FRESULT because FatFs is its filesystem, so the map is one hop and
 * lives here -- beside the calls that produce it, not in core, which never
 * sees a FRESULT.
 *
 * Named errmap and not errno because a host directory is on the include path
 * (core/emu.cmake puts it there so host.h can be reached), and a header
 * called errno.h there is the one every C library picks up instead of its
 * own. Renaming it back breaks every translation unit at once.
 */

#ifndef _RIA_API_ERRMAP_H_
#define _RIA_API_ERRMAP_H_

#include "core/api/api.h"

api_errno fresult_to_api(unsigned fresult);

#endif /* _RIA_API_ERRMAP_H_ */
