/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The read/write/close half of the POSIX fs seam, done synchronously. The
 * contract permits FS_IO_PENDING; it never requires it, and a transfer that
 * finished before it answered is a legal answer to the same question
 * fs_aio.c answers over several scanlines.
 *
 * This is what a host that lives inside another program's process takes.
 * glibc's POSIX AIO is a pool of helper threads, and a frontend closing a
 * core with one still in flight is a write into a library that has been
 * unmapped. There is nothing to reap here.
 */

#include "host/fs.h"
#include <unistd.h>

fs_io_result fs_read(int fd, char *buf, uint32_t count, uint32_t *got)
{
    *got = 0;
    ssize_t r = read(fd, buf, count);
    if (r < 0)
        return FS_IO_ERROR;
    *got = (uint32_t)r;
    return FS_IO_OK;
}

fs_io_result fs_write(int fd, const char *buf, uint32_t count, uint32_t *put)
{
    *put = 0;
    ssize_t r = write(fd, buf, count);
    if (r < 0)
        return FS_IO_ERROR;
    *put = (uint32_t)r;
    return FS_IO_OK;
}

int fs_close(int fd)
{
    return close(fd);
}
