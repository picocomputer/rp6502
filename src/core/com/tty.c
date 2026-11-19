/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/com/tty.h"
#include "core/com/com.h"
#include "core/com/manifold.h"
#include "core/str/str.h"
#include "machine.h"
#include <strings.h>

static bool tty_was_asked;

bool tty_std_handles(const char *path)
{
    return strcasecmp(path, STR_TTY_COLON) == 0;
}

int tty_std_open(const char *path, uint8_t mode, api_errno *err)
{
    (void)path;
    (void)mode;
    (void)err;
    return 0;
}

std_rw_result tty_std_close(int desc, api_errno *err)
{
    (void)desc;
    (void)err;
    return STD_OK;
}

std_rw_result tty_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err)
{
    (void)desc;
    (void)err;
    tty_was_asked = true;
    *bytes_read = (uint32_t)com_stdin_read(buf, count);
    return STD_OK;
}

std_rw_result tty_std_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err)
{
    (void)desc;
    (void)err;
    uint32_t i = 0;
    for (; i < count && manifold_cts(); i++)
        manifold_putchar_raw(buf[i]);
    *bytes_written = i;
    return STD_OK;
}

bool tty_asked(void)
{
    return tty_was_asked;
}

void HOST_IN_FLASH_FUNC tty_stop(void)
{
    tty_was_asked = false;
}

bool tty_std_ident(int desc, sst_cursor_t *c)
{
    (void)desc;
    (void)c;
    return true;
}

int tty_std_reopen(sst_cursor_t *c, uint8_t mode, api_errno *err)
{
    (void)c;
    (void)mode;
    (void)err;
    return 0;
}

void tty_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put_bool(c, tty_was_asked);
}

bool tty_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    bool asked = sst_get_bool(c);
    if (!sst_ok(c))
        return false;
    tty_was_asked = asked;
    return true;
}
