
/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _STR_H_
#define _STR_H_

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

bool char_is_hex(char ch);
int char_to_int(char ch);
int strnicmp(const char *string1, const char *string2, int n);
bool parse_uint32(const char **args, size_t *len, uint32_t *result);
bool parse_end(const char *args, size_t len);

#endif /* _STR_H_ */
