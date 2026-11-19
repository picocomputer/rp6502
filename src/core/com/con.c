/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/com/con.h"
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
#define CON_NOWAIT 3

static bool con_rln_active;
static const char *con_rln_buf;
static bool con_rln_needs_nl;
static size_t con_rln_pos;
static size_t con_rln_len;
static bool con_closed;
static bool con_was_asked;

static void HOST_IN_FLASH_FUNC con_rln_callback(bool timeout, const char *buf)
{
    (void)timeout;
    con_rln_active = false;
    con_rln_buf = buf;
    con_rln_pos = 0;
    con_rln_len = strlen(buf);
    con_rln_needs_nl = true;
}

rln_read_callback_t con_rln_reader(void)
{
    return con_rln_callback;
}

/* The cooked console: a line from the line editor, then its newline. It
 * waits for the line, which CON: with no number does not. */
static std_rw_result con_cooked_read(char *buf, uint32_t count, uint32_t *bytes_read)
{
    con_was_asked = true;
    *bytes_read = 0;
    if (count == 0)
        return STD_OK;
    if (!con_rln_needs_nl && con_rln_pos >= con_rln_len)
    {
        if (con_closed)
            return STD_OK;
        if (!con_rln_active)
        {
            con_rln_active = true;
            rln_read_line(con_rln_callback, NULL);
        }
        return STD_PENDING;
    }
    uint32_t i = 0;
    for (; i < count && con_rln_pos < con_rln_len; i++)
        buf[i] = con_rln_buf[con_rln_pos++];
    if (i < count && con_rln_needs_nl)
    {
        buf[i++] = '\n';
        con_rln_needs_nl = false;
    }
    *bytes_read = i;
    return STD_OK;
}

bool con_std_handles(const char *path)
{
    return strncasecmp(path, STR_CON_COLON, STR_CON_COLON_LEN) == 0;
}

int con_std_open(const char *path, uint8_t mode, api_errno *err)
{
    (void)mode;
    const char *n = path + STR_CON_COLON_LEN;
    if (!n[0])
        return CON_NOWAIT;
    if (n[0] >= '0' && n[0] <= '2' && !n[1])
        return n[0] - '0';
    *err = API_ENOENT;
    return -1;
}

std_rw_result con_std_close(int desc, api_errno *err)
{
    (void)desc;
    (void)err;
    return STD_OK;
}

std_rw_result con_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err)
{
    (void)err;
    std_rw_result result = con_cooked_read(buf, count, bytes_read);
    return (result == STD_PENDING && desc == CON_NOWAIT) ? STD_OK : result;
}

std_rw_result con_std_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err)
{
    (void)err;
    int to = desc == STDERR_FILENO ? STDERR_FILENO : STDOUT_FILENO;
    *bytes_written = (uint32_t)manifold_write(to, buf, count);
    return (*bytes_written < count && desc != CON_NOWAIT) ? STD_PENDING : STD_OK;
}

bool con_waiting(void)
{
    return con_rln_active;
}

bool con_asked(void)
{
    return con_was_asked;
}

void con_eof(void)
{
    con_closed = true;
    if (!con_rln_active)
        return;
    con_rln_active = false;
    /* A last line that the input never terminated is still a line, so it is
     * handed over before the read is given up; the next read is the one that
     * answers nothing. */
    if (!rln_read_flush())
        rln_read_cancel();
}

/* A stop ends the read in flight, which rln_stop gives up. The end of input
 * lasts until the end of the chain, so an exec does not read past it. */
void HOST_IN_FLASH_FUNC con_stop(void)
{
    con_rln_active = false;
    con_rln_needs_nl = false;
    con_rln_pos = 0;
    con_rln_len = 0;
    con_was_asked = false;
}

void HOST_IN_FLASH_FUNC con_end(void)
{
    con_closed = false;
}

#ifdef RP6502_SST

bool con_std_ident(int desc, sst_cursor_t *c)
{
    sst_put_u8(c, (uint8_t)desc);
    return true;
}

int con_std_reopen(sst_cursor_t *c, uint8_t mode, api_errno *err)
{
    (void)mode;
    uint8_t desc = sst_get_u8(c);
    if (!sst_ok(c) || desc > CON_NOWAIT)
    {
        *err = API_EINVAL;
        return -1;
    }
    return desc;
}

void con_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put_bool(c, con_rln_active);
    sst_put_bool(c, con_rln_needs_nl);
    sst_put_u16(c, (uint16_t)con_rln_pos);
    sst_put_u16(c, (uint16_t)con_rln_len);
    sst_put_bool(c, con_closed);
    sst_put_bool(c, con_was_asked);
}

bool con_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    bool active = sst_get_bool(c), needs_nl = sst_get_bool(c);
    uint16_t pos = sst_get_u16(c), len = sst_get_u16(c);
    bool closed = sst_get_bool(c), asked = sst_get_bool(c);
    if (!sst_ok(c) || pos > len || len >= RLN_LINE_MAX)
        return false;
    con_rln_active = active;
    con_rln_needs_nl = needs_nl;
    con_rln_pos = pos;
    con_rln_len = len;
    con_closed = closed;
    con_was_asked = asked;
    /* Not carried in the blob. It is either NULL or rln's line buffer, which
     * is at a fixed address, and nothing reads it until a line arrives. */
    con_rln_buf = rln_line();
    return true;
}

#endif /* RP6502_SST */
