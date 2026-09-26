/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _HOST_HOST_H_
#define _HOST_HOST_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    uint32_t host_seed(void);
    uint64_t host_clock_us(void);
    uint32_t host_crc32(uint32_t crc, const void *buf, size_t len);

    /* The SAVE: folder, as a host path in UTF-8, or NULL for a host that has
     * none, where SAVE: is the working directory when the ROM starts. */
    const char *host_save_dir(void);

#ifdef __cplusplus
}
#endif

#endif /* _HOST_HOST_H_ */
