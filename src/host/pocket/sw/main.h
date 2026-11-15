/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_MAIN_H_
#define _HOST_POCKET_SW_MAIN_H_

#include <stdint.h>
#include <stdbool.h>

#include "core/sys/driver.h"

/* main_upd_seen is restored from the blob with the rest of the RAM, but
 * no blob contains MMIO_UPD_N or MMIO_SLOT, so the values those registers
 * hold after a restore are marked as handled rather than read as a new
 * program. */
void main_restored(void);

/* Asks the main loop to stage the ROM in slot 0 and start it, as a boot
 * does. The start is made once no wake is in progress. */
void main_restage(void);

void tim_set_tz_minutes(int32_t min);

#endif /* _HOST_POCKET_SW_MAIN_H_ */
