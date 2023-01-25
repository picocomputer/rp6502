/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CMD_H_
#define _CMD_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

void cmd_task();
void cmd_reset();
bool cmd_is_active();
void cmd_binary(const char *args, size_t len);
void cmd_status(const char *args, size_t len);
void cmd_caps(const char *args, size_t len);
void cmd_start(const char *args, size_t len);
void cmd_resb(const char *args, size_t len);
void cmd_phi2(const char *args, size_t len);
void cmd_address(const char *args, size_t len);

#endif /* _CMD_H_ */
