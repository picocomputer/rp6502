
/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_MON_HELP_H_
#define _RIA_MON_HELP_H_

/* Monitor commands for help
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "ria/mon/mon.h"

/* Monitor commands
 */

void help_mon_help(const char *args);

// Look up help by category word plus optional sub-key.
const char *help_lookup(const char *word, const char *sub, mon_response_fn *fn);

// Test if help exists. Used to determine
// acceptable names when installing ROMs.
bool help_topic_exists(const char *buf);

#endif /* _RIA_MON_HELP_H_ */
