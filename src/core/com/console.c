/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/com/console.h"
#include "core/com/manifold.h"
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

static bool console_rln_active;
static const char *console_rln_buf;
static bool console_rln_needs_nl;
static size_t console_rln_pos;
static size_t console_rln_len;
static bool console_closed;
static bool console_was_asked;

static void HOST_IN_FLASH_FUNC console_rln_callback(bool timeout, const char *buf)
{
    (void)timeout;
    console_rln_active = false;
    console_rln_buf = buf;
    console_rln_pos = 0;
    console_rln_len = strlen(buf);
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
    uint32_t i = 0;
    for (; i < count && console_rln_pos < console_rln_len; i++)
        buf[i] = console_rln_buf[console_rln_pos++];
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
    *bytes_written = (uint32_t)manifold_write(to, buf, count);
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

void console_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put_bool(c, console_rln_active);
    sst_put_bool(c, console_rln_needs_nl);
    sst_put_u16(c, (uint16_t)console_rln_pos);
    sst_put_u16(c, (uint16_t)console_rln_len);
    sst_put_bool(c, console_closed);
    sst_put_bool(c, console_was_asked);
}

bool console_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    bool active = sst_get_bool(c), needs_nl = sst_get_bool(c);
    uint16_t pos = sst_get_u16(c), len = sst_get_u16(c);
    bool closed = sst_get_bool(c), asked = sst_get_bool(c);
    if (!sst_ok(c) || pos > len || len >= RLN_LINE_MAX)
        return false;
    console_rln_active = active;
    console_rln_needs_nl = needs_nl;
    console_rln_pos = pos;
    console_rln_len = len;
    console_closed = closed;
    console_was_asked = asked;
    /* Not carried in the blob. It is either NULL or rln's line buffer, which
     * is at a fixed address, and nothing reads it until a line arrives. */
    console_rln_buf = rln_line();
    return true;
}
