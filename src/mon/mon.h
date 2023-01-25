/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _MON_H_
#define _MON_H_

#include <stdbool.h>
#include <stdint.h>

void mon_task();
void mon_reset();
bool mon_command_exists(const char *buf, uint8_t buflen);

#endif /* _MON_H_ */
