/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_COM_TTY_H_
#define _CORE_COM_TTY_H_

/* TTY:, the console's keys and screen, raw, never waiting. Every machine's
 * stdio table has this row second, after CON:.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "core/api/std.h"
#include "core/sys/sst.h"

/* Main events
 */

void tty_stop(void);

/* True once the running program has read TTY:. It stays true until the
 * program stops, because a raw read completes at once and leaves nothing for
 * a caller to observe. */
bool tty_asked(void);

/* STDIO
 */

bool tty_std_handles(const char *path);
int tty_std_open(const char *path, uint8_t mode, api_errno *err);
std_rw_result tty_std_close(int desc, api_errno *err);
std_rw_result tty_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err);
std_rw_result tty_std_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err);
bool tty_std_ident(int desc, sst_cursor_t *c);
int tty_std_reopen(sst_cursor_t *c, uint8_t mode, api_errno *err);

#define TTY_SST_SIZE 1
void tty_sst_save(sst_cursor_t *c, unsigned flags);
bool tty_sst_load(sst_cursor_t *c, unsigned flags);

#define TTY_DRIVER DRIVER(            \
    STOP(tty_stop)                    \
    SST(.id = "TTY_",                 \
        .version = 1,                 \
        .size = TTY_SST_SIZE,         \
        .save = tty_sst_save,         \
        .load = tty_sst_load)         \
    STD(.handles = tty_std_handles,   \
        .open = tty_std_open,         \
        .close = tty_std_close,       \
        .read = tty_std_read,         \
        .write = tty_std_write,       \
        .terminal = true)             \
    STD_SST(.ident = tty_std_ident,   \
            .reopen = tty_std_reopen, \
            .resumes_writes = true))

#endif /* _CORE_COM_TTY_H_ */
