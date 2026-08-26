/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_API_OPS_H_
#define _CORE_API_OPS_H_

/* Which handler a 6502 syscall number reaches. Written once, because the
 * answer is the same on every machine: what differs between them is what a
 * handler finds underneath it -- a drive with no directories, a launcher that
 * starts a program differently -- and that difference is below this, not
 * here. */

#include <stdbool.h>
#include <stdint.h>

/* Run one op. False once it has returned to the 6502, true while it has more
 * to do and should be called again. An op with no handler is ENOSYS. */
bool ops_dispatch(uint8_t operation);

#endif /* _CORE_API_OPS_H_ */
