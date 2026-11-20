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
void console_reset(void);

/* Whether a cooked read is outstanding on the console, and the notice that
 * input has ended. On end of input whatever partial line was typed is still
 * delivered, so reads answer from what is left of it before every read answers
 * end of file until the exec chain ends. Telling it again is harmless.
 */

bool console_waiting(void);
void console_eof(void);

/* Everything written to the console has reached its terminals, the RIA's
 * copy included, and no smooth scroll is still moving: what a sync of the
 * console waits for, on CON: and TTY: alike. */
bool console_rendered(void);

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
std_rw_result console_std_sync(int desc, api_errno *err);
std_rw_result console_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err);
std_rw_result console_std_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err);
bool console_std_ident(int desc, sst_cursor_t *c);
int console_std_reopen(sst_cursor_t *c, uint8_t mode, api_errno *err);

extern const sst_table_t console_sst;

#define CONSOLE_DRIVER DRIVER(            \
    STOP(console_stop)                    \
    END(console_end)                      \
    RESET(console_reset)                  \
    SST("CON_", console)                  \
    STD(.handles = console_std_handles,   \
        .open = console_std_open,         \
        .close = console_std_close,       \
        .read = console_std_read,         \
        .write = console_std_write,       \
        .sync = console_std_sync,         \
        .terminal = true)                 \
    STD_SST(.ident = console_std_ident,   \
            .reopen = console_std_reopen, \
            .resumes_writes = true))


#endif /* _CORE_COM_CONSOLE_H_ */
