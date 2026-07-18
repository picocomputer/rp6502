/*
 * Copyright (c) 2026 Rumbledethumps
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
#include "api/api.h"

/* Main events
 */

void std_init(void);
void std_task(void);
void std_stop(void);

/* The API implementation for stdio support.
 */

bool std_api_open(void);
bool std_api_close(void);
bool std_api_read_xstack(void);
bool std_api_read_xram(void);
bool std_api_write_xstack(void);
bool std_api_write_xram(void);
bool std_api_syncfs(void);
bool std_api_lseek_cc65(void);
bool std_api_lseek_llvm(void);

/* Driver I/O result codes for read/write operations
 */

typedef enum
{
    STD_OK,      /* completed, success */
    STD_ERROR,   /* failed, check errno */
    STD_PENDING, /* incomplete, would block */
} std_rw_result;

// One stdio file driver. The open dispatcher claims a path with the first
// driver whose handles() returns true, so an inactive driver simply returns
// false. Each platform builds its own std_drivers[] table from this struct.
typedef struct
{
    // handles, open, and close are required
    bool (*handles)(const char *);
    int (*open)(const char *, uint8_t, api_errno *);
    // close and sync return STD_PENDING while draining (re-dispatched on
    // schedule), STD_OK when done, STD_ERROR on failure (check errno)
    std_rw_result (*close)(int desc, api_errno *);
    // everything else is optional
    std_rw_result (*read)(int desc, char *, uint32_t, uint32_t *, api_errno *);
    std_rw_result (*write)(int desc, const char *, uint32_t, uint32_t *, api_errno *);
    std_rw_result (*sync)(int desc, api_errno *);
    int (*lseek)(int desc, int8_t, int32_t, int32_t *, api_errno *);
} std_driver_t;

#endif /* _RIA_API_STD_H_ */
