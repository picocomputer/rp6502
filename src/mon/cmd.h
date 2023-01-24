/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CMD_H_
#define _CMD_H_

#include <stdint.h>
#include <stdbool.h>

void cmd_task();
void cmd_reset();
bool cmd_is_active();
char cmd_prompt();
void cmd_dispatch(const char *buf, uint8_t buflen);
bool cmd_exists(const char *buf, uint8_t buflen);

#endif /* _CMD_H_ */
