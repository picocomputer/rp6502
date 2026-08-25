
/*
 * Copyright (c) 2026 Rumbledethumps
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
#include "ria/mon/mon.h"

/* Monitor commands
 */

void hlp_mon_help(const char *args);

// Look up help by category word plus optional sub-key.
const char *hlp_lookup(const char *word, const char *sub, mon_response_fn *fn);

// Test if help exists. Used to determine
// acceptable names when installing ROMs.
bool hlp_topic_exists(const char *buf);

#endif /* _RIA_MON_HLP_H_ */
