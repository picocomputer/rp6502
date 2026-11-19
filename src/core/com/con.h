/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_COM_CON_H_
#define _CORE_COM_CON_H_

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

void con_stop(void);
void con_end(void);

/* Whether a cooked read is outstanding on the console, and the notice that
 * input has ended. On end of input whatever partial line was typed is still
 * delivered, so reads answer from what is left of it before every read answers
 * end of file until the exec chain ends. Telling it again is harmless.
 */

bool con_waiting(void);
void con_eof(void);

/* True once the running program has read CON:. It stays true until the
 * program stops. */
bool con_asked(void);

/* The reader rln calls back with a line. rln writes down whether a read was
 * outstanding as a token rather than as a pointer, so this is the reader its
 * load installs when the token says one was. */
rln_read_callback_t con_rln_reader(void);

/* STDIO
 */

bool con_std_handles(const char *path);
int con_std_open(const char *path, uint8_t mode, api_errno *err);
std_rw_result con_std_close(int desc, api_errno *err);
std_rw_result con_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err);
std_rw_result con_std_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err);
bool con_std_ident(int desc, sst_cursor_t *c);
int con_std_reopen(sst_cursor_t *c, uint8_t mode, api_errno *err);

/* How far the cooked reader has got through a line, and the two flags. */
#define CON_SST_SIZE 8
void con_sst_save(sst_cursor_t *c, unsigned flags);
bool con_sst_load(sst_cursor_t *c, unsigned flags);

#define CON_DRIVER DRIVER( \
    STOP(con_stop)         \
    END(con_end)           \
    SST(CON_, 1, CON_SST_SIZE, con_sst_save, con_sst_load))

/* The desc of CON:0, CON:1 and CON:2 is the number, so std_reset opens the
 * console on 0, 1 and 2 with this row and the descriptor for its desc. */
#define CON_STD_DRIVER                                        \
    {                                                         \
        .handles = con_std_handles,                           \
        .open = con_std_open,                                 \
        .close = con_std_close,                               \
        .read = con_std_read,                                 \
        .write = con_std_write,                               \
        STD_SST_COLS(con_std_ident, con_std_reopen, true)     \
    }

#endif /* _CORE_COM_CON_H_ */
