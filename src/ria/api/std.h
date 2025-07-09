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

void std_stop(void);

/* The API implementation for stdio support.
 */

bool std_api_open(void);
bool std_api_close(void);
bool std_api_read_xstack(void);
bool std_api_read_xram(void);
bool std_api_write_xstack(void);
bool std_api_write_xram(void);
bool std_api_lseek(void);
bool std_api_unlink(void);
bool std_api_rename(void);

#endif /* _STD_H_ */
