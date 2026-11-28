/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* What src/host/fpga/sw asks of the Pocket, src/host/fpga/README.md's host
 * seam. crt0.S includes it too, so only the version is seen by the
 * assembler. */

#ifndef _HOST_POCKET_FPGA_HOST_H_
#define _HOST_POCKET_FPGA_HOST_H_

#include "pocket_version.h"

/* The register layout version crt0.S carries after its reset jump. */
#define FPGA_REG_VERSION POCKET_REG_VERSION

#ifndef __ASSEMBLER__

#include "sw/bridge.h"

#include <stdbool.h>
#include <stdint.h>

/* The RTC reading is the local time in seconds since 1970, from APF
 * command 0x0090, which the host sends once at core boot and which sets
 * RTC_VALID. The command has no time zone field. */
static inline bool fpga_rtc_valid(void)
{
    return RTC_VALID != 0;
}

static inline uint32_t fpga_rtc_epoch(void)
{
    return RTC_EPOCH;
}

/* The UTC offset in minutes, east positive, from the interact menu. */
static inline int32_t fpga_tz_minutes(void)
{
    return set_tz_minutes();
}

/* A position in def/keyboard.def plus one, 0 for none yet. */
static inline int32_t fpga_kb_layout(void)
{
    return (int32_t)SET_KB;
}

#endif /* __ASSEMBLER__ */

#endif /* _HOST_POCKET_FPGA_HOST_H_ */
