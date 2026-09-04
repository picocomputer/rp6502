/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This machine's proc: its row over core/api/proc.c, and the one way into a
 * program that is its own -- an NFC tag naming a ROM. The load itself is
 * ria/mon/rom.c's task.
 */

#ifndef _RIA_API_PROC_H_
#define _RIA_API_PROC_H_

#include "core/api/proc.h"

#include <stddef.h>
#include <stdint.h>

/* Boot the ROM an NFC tag names. */
void proc_nfc(const uint8_t *data, size_t len);

/* This machine's proc row; see core/sys/driver.h. */
#define PROC_DRIVER DRIVER(nul_init, nul_task, nul_task, proc_run, proc_stop, nul_break, nul_config, nul_config)

#endif /* _RIA_API_PROC_H_ */
