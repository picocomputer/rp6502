/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _STD_H_
#define _STD_H_

#include <stdbool.h>

/* Kernel events
 */

void std_task(void);
void std_stop(void);

// Active until stdout buffer empty.
bool std_active(void);

/* The API implementation for stdio support.
 */

void std_api_open(void);
void std_api_close(void);
void std_api_read_xstack(void);
void std_api_read_xram(void);
void std_api_write_xstack(void);
void std_api_write_xram(void);
void std_api_lseek(void);
void std_api_unlink(void);
void std_api_rename(void);

#endif /* _STD_H_ */
