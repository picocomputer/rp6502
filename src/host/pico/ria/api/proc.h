/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_API_PROC_H_
#define _RIA_API_PROC_H_

#include "core/api/proc.h"

#include <stddef.h>
#include <stdint.h>

void proc_nfc(const uint8_t *data, size_t len);

#define PROC_DRIVER DRIVER(RUN(proc_run) STOP(proc_stop))

#endif /* _RIA_API_PROC_H_ */
