/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_API_ARG_H_
#define _RIA_API_ARG_H_

/* The on-wire argv buffer the 6502 exchanges through the argv/exec API ops. This
 * owns the argv layout (offset table + packed strings) so firmware and emulator
 * share one definition of the guest ABI.
 */

#include <stdint.h>
#include <stdbool.h>

void arg_clear(void);
bool arg_append(const char *str);
bool arg_replace(uint16_t idx, const char *str);
const char *arg_index(uint16_t idx);

// op 0x08: copy the argv onto the xstack; returns its byte size.
uint16_t arg_push_xstack(void);
// op 0x09: load the argv from the xstack and validate it.
bool arg_pull_xstack(void);

#endif /* _RIA_API_ARG_H_ */
