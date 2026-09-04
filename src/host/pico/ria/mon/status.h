/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_MON_STATUS_H_
#define _RIA_MON_STATUS_H_

/* What this machine says it is. The boot banner and the STATUS command are the
 * same list, one of them cut short: the name, this build, and then whatever
 * each piece of hardware answers about itself. */

/* Queue the banner. mon_init asks, before anything can queue an error. */
void status_add_boot_response(void);

/* Monitor command
 */

void status_mon_status(const char *args);

#endif /* _RIA_MON_STATUS_H_ */
