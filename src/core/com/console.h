/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_COM_CONSOLE_H_
#define _CORE_COM_CONSOLE_H_

/* CON:, the console as stdio has it, cooked through the line editor. Every
 * machine's stdio table starts with this row, then TTY:'s.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "core/api/std.h"
#include "core/com/rln.h"
#include "core/sys/sst.h"

/* Main events
 */

void console_stop(void);
void console_end(void);

/* Whether a cooked read is outstanding on the console, and the notice that
 * input has ended. On end of input whatever partial line was typed is still
 * delivered, so reads answer from what is left of it before every read answers
 * end of file until the exec chain ends. Telling it again is harmless.
 */

bool console_waiting(void);
void console_eof(void);

/* True once the running program has read CON:. It stays true until the
 * program stops. */
bool console_asked(void);

/* The reader rln calls back with a line. rln writes down whether a read was
 * outstanding as a token rather than as a pointer, so this is the reader its
 * load installs when the token says one was. */
rln_read_callback_t console_rln_reader(void);

/* STDIO
 */

bool console_std_handles(const char *path);
int console_std_open(const char *path, uint8_t mode, api_errno *err);
std_rw_result console_std_close(int desc, api_errno *err);
std_rw_result console_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err);
std_rw_result console_std_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err);
bool console_std_ident(int desc, sst_cursor_t *c);
int console_std_reopen(sst_cursor_t *c, uint8_t mode, api_errno *err);

/* How far the cooked reader has got through a line, and the two flags. */
#define CONSOLE_SST_SIZE 8
void console_sst_save(sst_cursor_t *c, unsigned flags);
bool console_sst_load(sst_cursor_t *c, unsigned flags);

#define CONSOLE_DRIVER DRIVER(            \
    STOP(console_stop)                    \
    END(console_end)                      \
    SST(.id = "CON_",                     \
        .version = 1,                     \
        .size = CONSOLE_SST_SIZE,         \
        .save = console_sst_save,         \
        .load = console_sst_load)         \
    STD(.handles = console_std_handles,   \
        .open = console_std_open,         \
        .close = console_std_close,       \
        .read = console_std_read,         \
        .write = console_std_write,       \
        .terminal = true)                 \
    STD_SST(.ident = console_std_ident,   \
            .reopen = console_std_reopen, \
            .resumes_writes = true))


#endif /* _CORE_COM_CONSOLE_H_ */
