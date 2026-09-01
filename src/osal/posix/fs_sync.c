/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The read/write/close slots of the POSIX file driver, done synchronously.
 * The contract permits STD_PENDING; it never requires it, and a transfer that
 * finished before it answered is a legal answer to the same question
 * fs_aio.c answers over several scanlines.
 *
 * This is what a host that lives inside another program's process takes.
 * glibc's POSIX AIO is a pool of helper threads, and a frontend closing a
 * core with one still in flight is a write into a library that has been
 * unmapped. There is nothing to reap here.
 */

#include "osal/fs.h"
#include "osal/posix/errmap.h"
#include <errno.h>
#include <unistd.h>

std_rw_result fs_std_read(int desc, char *buf, uint32_t count, uint32_t *got, api_errno *err)
{
    *got = 0;
    ssize_t r = read(desc, buf, count);
    if (r < 0)
    {
        *err = errno_to_api(errno);
        return STD_ERROR;
    }
    *got = (uint32_t)r;
    return STD_OK;
}

std_rw_result fs_std_write(int desc, const char *buf, uint32_t count, uint32_t *put, api_errno *err)
{
    *put = 0;
    ssize_t r = write(desc, buf, count);
    if (r < 0)
    {
        *err = errno_to_api(errno);
        return STD_ERROR;
    }
    *put = (uint32_t)r;
    return STD_OK;
}

std_rw_result fs_std_close(int desc, api_errno *err)
{
    if (close(desc) != 0) /* also a deferred flush failure: ENOSPC, EIO */
    {
        *err = errno_to_api(errno);
        return STD_ERROR;
    }
    return STD_OK;
}
