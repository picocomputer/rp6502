/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What a Pico firmware says about itself. Both compile this; the VGA takes
 * only the clock, and the seed and the CRC go unreferenced there, so the
 * linker drops them along with the littlefs they would have reached.
 */

#include "host/host.h"
#include "osal/os.h"

#include <littlefs/lfs_util.h>
#include <pico/time.h> /* time_us_64 */

/* TIMER0's raw 64-bit microsecond counter, free-running since reset. */
uint64_t host_clock_us(void)
{
    return time_us_64();
}

uint32_t host_random_seed(void)
{
    /* Nothing overrides a board: no command line, no fixture. */
    return os_random_entropy();
}

uint32_t host_crc32(uint32_t crc, const void *buf, size_t len)
{
    // littlefs's CRC table is already linked; reuse it, don't add a second.
    return ~lfs_crc(~crc, buf, len);
}
