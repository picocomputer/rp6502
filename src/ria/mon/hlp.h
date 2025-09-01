
/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_MON_HLP_H_
#define _RIA_MON_HLP_H_

/* Monitor commands for help
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Monitor commands
 */

void hlp_mon_help(const char *args, size_t len);

// Test if help exists. Used to determine
// acceptable names when installing ROMs.
bool hlp_topic_exists(const char *buf, size_t buflen);

#endif /* _RIA_MON_HLP_H_ */
