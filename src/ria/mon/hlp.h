
/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HLP_H_
#define _HLP_H_

#include <stddef.h>

/* Monitor commands
 */

void hlp_mon_help(const char *args, size_t len);
const char *help_text_lookup(const char *args, size_t len);

#endif /* _HLP_H_ */
