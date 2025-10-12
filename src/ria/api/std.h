/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_API_STD_H_
#define _RIA_API_STD_H_

/* Provides STDIO to the 6502.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void std_run(void);
void std_stop(void);

/* The API implementation for stdio support.
 */

bool std_api_open(void);
bool std_api_close(void);
bool std_api_read_xstack(void);
bool std_api_read_xram(void);
bool std_api_write_xstack(void);
bool std_api_write_xram(void);
bool std_api_lseek_cc65(void);
bool std_api_lseek_llvm(void);
bool std_api_sync(void);
bool std_api_stdin_opt(void);

#endif /* _RIA_API_STD_H_ */
