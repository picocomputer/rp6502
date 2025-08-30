/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_NET_CMD_H_
#define _RIA_NET_CMD_H_

/* Parser of Hayes-like commands.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// We have one job.
bool cmd_parse(const char **s);

#endif /* _RIA_NET_CMD_H_ */
