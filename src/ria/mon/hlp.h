
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
#include "mon/mon.h"

/* Monitor commands
 */

typedef struct
{
    const char *prose;   // primary help text, NULL when the topic is unknown
    const char *append;  // extra block queued after prose, or NULL
    mon_response_fn fn;  // list responder queued after prose, or NULL
} hlp_topic_t;

void hlp_mon_help(const char *args);

// Look up help by category word plus optional sub-key. word is a command
// (e.g. STR_COPY) or STR_SET / STR_DISK / STR_ABOUT / STR_CREDITS; sub is the
// SET attribute or DISK subcommand, or NULL. Returns false (out cleared) if the
// topic is unknown.
bool hlp_lookup(const char *word, const char *sub, hlp_topic_t *out);

// Test if help exists. Used to determine
// acceptable names when installing ROMs.
bool hlp_topic_exists(const char *buf);

#endif /* _RIA_MON_HLP_H_ */
