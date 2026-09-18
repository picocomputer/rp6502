/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_MAIN_H_
#define _HOST_POCKET_SW_MAIN_H_

/* main_upd_seen is restored from the blob with the rest of the TCM, but
 * no blob contains MMIO_UPD_N or MMIO_SLOT, so the values those registers
 * hold after a restore are marked as handled rather than read as a new
 * program. */
void main_restored(void);
void main_wake_failed(void);

#include <stdint.h>
#include <stdbool.h>

#include "core/sys/driver.h"

extern bool main_boot_wake;
extern uint32_t main_boot_slot;
extern uint8_t main_boot_upd;

void tim_set_tz_minutes(int32_t min);

#endif /* _HOST_POCKET_SW_MAIN_H_ */
