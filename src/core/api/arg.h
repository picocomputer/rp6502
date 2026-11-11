/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_API_ARG_H_
#define _CORE_API_ARG_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

void arg_clear(void);
bool arg_append(const char *str);
bool arg_replace(uint16_t idx, const char *str);
const char *arg_index(uint16_t idx);

uint16_t arg_push_xstack(void);
/* Takes the ARGV data on the XSTACK as the arguments when its layout is valid,
 * it has at least one string, and the first is at most first_max bytes. False
 * leaves the arguments as they were. The XSTACK is emptied either way. */
bool arg_pull_xstack(uint16_t first_max);

/* The whole buffer, for a savestate. The offsets inside it are stored little
 * endian whatever the host is, so the bytes carry no host layout. */
size_t arg_bytes(void);
const uint8_t *arg_data(void);
void arg_set_data(const uint8_t *buf);

#endif /* _CORE_API_ARG_H_ */
