/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* FatFs and littlefs failures, in the API's words.
 */

#ifndef _OSAL_PICO_ERRMAP_H_
#define _OSAL_PICO_ERRMAP_H_

#include "core/api/api.h"

api_errno fresult_to_api(unsigned fresult);
api_errno lfs_error_to_api(int lfs_err);

#endif /* _OSAL_PICO_ERRMAP_H_ */
