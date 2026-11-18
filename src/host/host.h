/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _HOST_HOST_H_
#define _HOST_HOST_H_

#include <stdbool.h>
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

    /* Whether the screen is in a window that is minimized, for the
     * console's answer to CSI 11 t. A machine with a display of its own
     * says no. */
    bool host_minimized(void);

#ifdef __cplusplus
}
#endif

#endif /* _HOST_HOST_H_ */
