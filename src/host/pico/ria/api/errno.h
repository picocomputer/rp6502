/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* What went wrong, in the API's words. This machine's failures arrive as a
 * FatFs FRESULT because FatFs is its filesystem, so the map is one hop and
 * lives here -- beside the calls that produce it, not in core, which never
 * sees a FRESULT.
 */

#ifndef _RIA_API_ERRNO_H_
#define _RIA_API_ERRNO_H_

#include "core/api/api.h"

api_errno fresult_to_api(unsigned fresult);

#endif /* _RIA_API_ERRNO_H_ */
