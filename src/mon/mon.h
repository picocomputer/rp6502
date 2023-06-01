/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _MON_H_
#define _MON_H_

#include <stdbool.h>
#include <stdint.h>

bool mon_command_exists(const char *buf, uint8_t buflen);
void mon_task();
void mon_reset();

#endif /* _MON_H_ */
