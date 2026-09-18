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

#define PROC_DRIVER DRIVER(nul_init, nul_task, nul_task, proc_run, proc_stop, nul_break, nul_config, nul_config, nul_sst)

#endif /* _RIA_API_PROC_H_ */
