/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/host/aio.h"
#include <errno.h>
#include <string.h>

#ifdef EMU_HAVE_AIO

static bool g_enabled;

void aio_set_enabled(bool on) { g_enabled = on; }
bool aio_enabled(void) { return g_enabled; }

bool aio_active(const struct aio_slot *s) { return s->active; }

bool aio_submit_read(struct aio_slot *s, int fd, int64_t off, void *buf, size_t n)
{
    memset(&s->cb, 0, sizeof s->cb);
    s->cb.aio_fildes = fd;
    s->cb.aio_offset = (off_t)off;
    s->cb.aio_buf = buf;
    s->cb.aio_nbytes = n;
    s->cb.aio_sigevent.sigev_notify = SIGEV_NONE;
    if (aio_read(&s->cb) != 0)
        return false;
    s->active = true;
    return true;
}

bool aio_submit_write(struct aio_slot *s, int fd, int64_t off, const void *buf, size_t n)
{
    memset(&s->cb, 0, sizeof s->cb);
    s->cb.aio_fildes = fd;
    s->cb.aio_offset = (off_t)off;
    s->cb.aio_buf = (void *)buf;
    s->cb.aio_nbytes = n;
    s->cb.aio_sigevent.sigev_notify = SIGEV_NONE;
    if (aio_write(&s->cb) != 0)
        return false;
    s->active = true;
    return true;
}

int aio_poll(struct aio_slot *s, size_t *got)
{
    int e = aio_error(&s->cb);
    if (e == EINPROGRESS)
        return 0;
    s->active = false;
    ssize_t r = aio_return(&s->cb);
    if (r < 0)
    {
        errno = e; /* the async failure, not aio_return's own errno write */
        return -1;
    }
    *got = (size_t)r;
    return 1;
}

void aio_slot_cancel(struct aio_slot *s, int fd)
{
    if (!s->active)
        return;
    const struct aiocb *l = &s->cb;
    aio_cancel(fd, &s->cb);
    while (aio_error(&s->cb) == EINPROGRESS)
        aio_suspend(&l, 1, NULL);
    aio_return(&s->cb);
    s->active = false;
}

#else /* !EMU_HAVE_AIO: inert; callers fall back to synchronous fs_* I/O */

// TODO this is a MAJOR DEFECT
// nothing should be using synchronous fs IO on any platform.

void aio_set_enabled(bool on) { (void)on; }
bool aio_enabled(void) { return false; }
bool aio_active(const struct aio_slot *s) { return (void)s, false; }

bool aio_submit_read(struct aio_slot *s, int fd, int64_t off, void *buf, size_t n)
{
    return (void)s, (void)fd, (void)off, (void)buf, (void)n, false;
}

bool aio_submit_write(struct aio_slot *s, int fd, int64_t off, const void *buf, size_t n)
{
    return (void)s, (void)fd, (void)off, (void)buf, (void)n, false;
}

int aio_poll(struct aio_slot *s, size_t *got)
{
    return (void)s, (void)got, -1;
}

void aio_slot_cancel(struct aio_slot *s, int fd) { (void)s, (void)fd; }

#endif
