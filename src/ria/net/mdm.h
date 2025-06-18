/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _MDM_H_
#define _MDM_H_

#include "net/nvr.h"

void mdm_task(void);
void mdm_stop(void);
void mdm_init(void);

bool mdm_open(const char *);
bool mdm_close(void);

int mdm_rx(char *ch);
int mdm_tx(char ch);

extern nvr_settings_t mdm_settings;
int mdm_response_code(char *buf, size_t buf_size, int state);
void mdm_set_response_fn(int (*fn)(char *, size_t, int), int state);

#endif /* _MDM_H_ */
