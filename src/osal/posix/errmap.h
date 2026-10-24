/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* POSIX errno, in the API's words.
 */

#ifndef _OSAL_POSIX_ERRMAP_H_
#define _OSAL_POSIX_ERRMAP_H_

#include "core/api/api.h"

api_errno errno_to_api(int host_errno);

#endif /* _OSAL_POSIX_ERRMAP_H_ */
