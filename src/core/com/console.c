/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/com/console.h"
#include "core/com/manifold.h"
#include "core/com/rln.h"
#include "core/term/cells.h"
#include "core/str/str.h"
#include "machine.h"
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* CON:0, CON:1 and CON:2 are the console as stdin, stdout and stderr have
 * it, and their desc is the number: each reads and writes, and waits, and
 * what CON:2 writes is stderr where a machine keeps the two apart. CON: alone
 * is the console that never waits, as it always was. */
#define CONSOLE_NOWAIT 3

/* The line rln handed over is read from rln_line, where it stays until the
 * next read, which starts only once all of it has been read. */
static bool console_rln_active;
static bool console_rln_needs_nl;
static uint16_t console_rln_pos;
static uint16_t console_rln_len;
static bool console_closed;
static bool console_was_asked;

static bool console_sst_valid(sst_cursor_t *c)
{
    if (console_rln_pos > console_rln_len ||
        console_rln_len >= RLN_LINE_MAX)
        return sst_refuse(c, "a line longer than rln's buffer");
    return true;
}

#define CONSOLE_SST_ROWS(X)              \
    X(BOOL, console_rln_active)          \
    X(BOOL, console_rln_needs_nl)        \
    X(U16, console_rln_pos)              \
    X(U16, console_rln_len)              \
    X(BOOL, console_closed)              \
    X(BOOL, console_was_asked)           \
    X(VALIDATE, console_sst_valid)
SST_TABLE(console, CONSOLE_SST_ROWS)

static void HOST_IN_FLASH_FUNC console_rln_callback(bool timeout, const char *buf)
{
    (void)timeout;
    console_rln_active = false;
    console_rln_pos = 0;
    console_rln_len = (uint16_t)strlen(buf);
    console_rln_needs_nl = true;
}

rln_read_callback_t console_rln_reader(void)
{
    return console_rln_callback;
}

/* The cooked console: a line from the line editor, then its newline. It
 * waits for the line, which CON: with no number does not. */
static std_rw_result console_cooked_read(char *buf, uint32_t count, uint32_t *bytes_read)
{
    console_was_asked = true;
    *bytes_read = 0;
    if (count == 0)
        return STD_OK;
    /* A line the editor ended is handed over once its echo has gone out. */
    if (rln_replying())
        return STD_PENDING;
    if (!console_rln_needs_nl && console_rln_pos >= console_rln_len)
    {
        if (console_closed)
            return STD_OK;
        if (!console_rln_active)
        {
            console_rln_active = true;
            rln_read_line(console_rln_callback, NULL);
        }
        return STD_PENDING;
    }
    const char *line = rln_line();
    uint32_t i = 0;
    for (; i < count && console_rln_pos < console_rln_len; i++)
        buf[i] = line[console_rln_pos++];
    if (i < count && console_rln_needs_nl)
    {
        buf[i++] = '\n';
        console_rln_needs_nl = false;
    }
    *bytes_read = i;
    return STD_OK;
}

bool console_std_handles(const char *path)
{
    return strncasecmp(path, STR_CON_COLON, STR_CON_COLON_LEN) == 0;
}

int console_std_open(const char *path, uint8_t mode, api_errno *err)
{
    (void)mode;
    const char *n = path + STR_CON_COLON_LEN;
    if (!n[0])
        return CONSOLE_NOWAIT;
    if (n[0] >= '0' && n[0] <= '2' && !n[1])
        return n[0] - '0';
    *err = API_ENOENT;
    return -1;
}

std_rw_result console_std_close(int desc, api_errno *err)
{
    (void)desc;
    (void)err;
    return STD_OK;
}

bool console_rendered(void)
{
    return manifold_drained() && !cells_smooth_busy(term_shown(&cells_term));
}

/* A sync waits until what was written shows: a program that wants its last
 * lines to finish a smooth scroll syncs before it exits. */
std_rw_result console_std_sync(int desc, api_errno *err)
{
    (void)desc;
    (void)err;
    return console_rendered() ? STD_OK : STD_PENDING;
}

std_rw_result console_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err)
{
    (void)err;
    std_rw_result result = console_cooked_read(buf, count, bytes_read);
    return (result == STD_PENDING && desc == CONSOLE_NOWAIT) ? STD_OK : result;
}

std_rw_result console_std_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err)
{
    (void)err;
    int to = desc == STDERR_FILENO ? STDERR_FILENO : STDOUT_FILENO;
    /* The line editor's reply to a key goes out whole, and the program's
     * output waits behind it. */
    *bytes_written = rln_replying() ? 0 : (uint32_t)manifold_write(to, buf, count);
    return (*bytes_written < count && desc != CONSOLE_NOWAIT) ? STD_PENDING : STD_OK;
}

bool console_waiting(void)
{
    return console_rln_active;
}

bool console_asked(void)
{
    return console_was_asked;
}

void console_eof(void)
{
    console_closed = true;
    if (!console_rln_active)
        return;
    console_rln_active = false;
    /* A last line that the input never terminated is still a line, so it is
     * handed over before the read is given up; the next read is the one that
     * answers nothing. */
    if (!rln_read_flush())
        rln_read_cancel();
}

/* A stop ends the read in flight, which rln_stop gives up. The end of input
 * lasts until the end of the chain, so an exec does not read past it. */
void HOST_IN_FLASH_FUNC console_stop(void)
{
    console_rln_active = false;
    console_rln_needs_nl = false;
    console_rln_pos = 0;
    console_rln_len = 0;
    console_was_asked = false;
}

void HOST_IN_FLASH_FUNC console_end(void)
{
    console_closed = false;
}

/* A failed load can leave a read waiting on a line rln will never hand
 * over, a line longer than rln's buffer, or the input closed. Neither STOP
 * nor END runs then, so this does both. */
void console_reset(void)
{
    console_stop();
    console_end();
}

bool console_std_ident(int desc, sst_cursor_t *c)
{
    sst_put_u8(c, (uint8_t)desc);
    return true;
}

int console_std_reopen(sst_cursor_t *c, uint8_t mode, api_errno *err)
{
    (void)mode;
    uint8_t desc = sst_get_u8(c);
    if (!sst_ok(c) || desc > CONSOLE_NOWAIT)
    {
        *err = API_EINVAL;
        return -1;
    }
    return desc;
}
