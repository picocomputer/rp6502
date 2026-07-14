/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Windows directory enumeration (emu/plat.h dir_*), the Win32 counterpart of
 * posix/dir.c.
 *
 * Paths cross the seam in the guest's OEM code page. Convert to UTF-16 with
 * oem_to_wide() (emu/api/oem.h) before every …W call, and convert returned names
 * back with oem_from_wide(). There is no opendir/readdir on Win32; use
 * FindFirstFileW/FindNextFileW/FindClose over an opaque heap struct.
 */

#include "emu/plat.h"
#include "emu/api/oem.h"
#include "emu/win/win.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

struct win_dir
{
    HANDLE h;
    WIN32_FIND_DATAW fd;
    bool first; /* FindFirstFileW already yielded the first entry */
    bool alive;
    wchar_t pattern[WIN_WPATH_MAX];
};

void *dir_open(const char *path)
{
    wchar_t base[WIN_WPATH_MAX];
    if (oem_to_wide(path, (uint16_t *)base, WIN_WPATH_MAX) <= 0)
    {
        errno = EINVAL;
        return NULL;
    }
    size_t n = wcslen(base);
    while (n > 0 && (base[n - 1] == L'\\' || base[n - 1] == L'/'))
        base[--n] = 0;

    struct win_dir *d = (struct win_dir *)calloc(1, sizeof(*d));
    if (!d)
    {
        errno = ENOMEM;
        return NULL;
    }
    if (n + 3 >= WIN_WPATH_MAX)
    {
        free(d);
        errno = ENAMETOOLONG;
        return NULL;
    }
    memcpy(d->pattern, base, (n + 1) * sizeof(wchar_t));
    d->pattern[n++] = L'\\';
    d->pattern[n++] = L'*';
    d->pattern[n] = 0;

    d->h = FindFirstFileW(d->pattern, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE)
    {
        win_set_errno(GetLastError());
        free(d);
        return NULL;
    }
    d->first = true;
    d->alive = true;
    return d;
}

int dir_read(void *opaque, char *name, size_t namesz, bool *is_dir)
{
    struct win_dir *d = (struct win_dir *)opaque;
    if (!d || !d->alive)
    {
        errno = EBADF;
        return -1;
    }
    if (!d->first)
    {
        if (!FindNextFileW(d->h, &d->fd))
        {
            DWORD e = GetLastError();
            if (e == ERROR_NO_MORE_FILES)
                return 0;
            win_set_errno(e);
            return -1;
        }
    }
    d->first = false;
    *is_dir = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    oem_from_wide((const uint16_t *)d->fd.cFileName, name, namesz);
    return 1;
}

void dir_rewind(void *opaque)
{
    struct win_dir *d = (struct win_dir *)opaque;
    if (!d)
        return;
    if (d->alive && d->h != INVALID_HANDLE_VALUE)
        FindClose(d->h);
    d->h = FindFirstFileW(d->pattern, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE)
    {
        d->alive = false;
        return;
    }
    d->first = true;
    d->alive = true;
}

void dir_close(void *opaque)
{
    struct win_dir *d = (struct win_dir *)opaque;
    if (!d)
        return;
    if (d->alive && d->h != INVALID_HANDLE_VALUE)
        FindClose(d->h);
    free(d);
}
