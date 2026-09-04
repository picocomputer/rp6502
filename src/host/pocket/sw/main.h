/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_MAIN_H_
#define _HOST_POCKET_SW_MAIN_H_

/* The restage triggers re-synced after a restore, so a wake's fresh
 * host announcements do not read as a new program. */
void main_restored(void);
/* The restore that the wake boot declined to stage a ROM for is not
 * coming. There is no session to keep, so this boot has to do the
 * staging it skipped. */
void main_wake_failed(void);

#include <stdint.h>
#include <stdbool.h>

#include "core/sys/driver.h"

/* What main() saw at boot, said later: the moment it is knowable is the
 * moment the host may be streaming a blob in, and the console competes
 * with that stream for the staging store. */
extern bool main_boot_wake;
extern uint32_t main_boot_slot;
extern uint8_t main_boot_upd;

/* This platform's time zone is the menu's UTC offset in signed
 * minutes, not a POSIX string; time.c turns it into one for the C
 * library. cfg_task feeds changes here. */
void tim_set_tz_minutes(int32_t min);

#endif /* _HOST_POCKET_SW_MAIN_H_ */
