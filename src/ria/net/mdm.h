/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _MDM_H_
#define _MDM_H_

#include <stdbool.h>

void mdm_task(void);
void mdm_reset(void);
void mdm_init(void);

bool mdm_open(const char *);
bool mdm_close(void);

#endif /* _MDM_H_ */
