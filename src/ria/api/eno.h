/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_API_ENO_H_
#define _RIA_API_ENO_H_

/* This allows selection of retured errno because
 * cc65 and llvm-mos have different errno.h constants.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

void eno_run(void);
bool eno_api_errno_opt(void);

uint16_t eno_posix(unsigned num);
uint16_t eno_fatfs(unsigned fresult);

#endif /* _RIA_API_ENO_H_ */
