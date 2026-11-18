/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The size of every blob this machine makes, as a constant. It walks the
 * machine's driver list, so it includes machine.h and drivers.h, and only a
 * file built for one machine includes it: core/sys/sst.c, and a machine that
 * checks the size against a limit of its own. sst_size() answers the same
 * number for everyone else. */

#ifndef _CORE_SYS_SST_SIZE_H_
#define _CORE_SYS_SST_SIZE_H_

/* A driver's slot size may be built from the machine's own constants, so
 * machine.h is included before the driver list in drivers.h. */
#include "machine.h"
#include "drivers.h"

/* magic 4, format 2, count 2, total 4, manifest 4, latch 2 */
#define SST_HEADER_LEN 18
/* id 4, version 2, used 4 */
#define SST_CHUNK_HDR 10
/* payload sum 4, end magic 4 */
#define SST_TRAILER_LEN 8

#define SST(id, ver, size, sv, ld) +(SST_CHUNK_HDR + (size))
enum
{
    SST_PAYLOAD_LEN = 0
#include "core/sys/driver_walk.h"
        DRIVERS_FORWARD(RP6502_MACH_DRIVERS)
#include "core/sys/driver_walk_end.h"
};
#define SST(id, ver, size, sv, ld) +1
enum
{
    SST_COUNT = 0
#include "core/sys/driver_walk.h"
        DRIVERS_FORWARD(RP6502_MACH_DRIVERS)
#include "core/sys/driver_walk_end.h"
};

#define SST_TOTAL (SST_HEADER_LEN + SST_PAYLOAD_LEN + SST_TRAILER_LEN)

#endif /* _CORE_SYS_SST_SIZE_H_ */
