/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Windows directory enumeration (host/api/dir.h host_dir_*), the Win32 counterpart
 * of host/posix/dir.c.
 *
 * Paths cross the seam spelled the way the 6502 spells them and in its OEM
 * code page. The drive prefix comes off with path_to_native() and the code
 * page with oem_to_wide() (core/str/oem.h) before every …W call; returned
 * names go back with oem_from_wide(). There is no opendir/readdir on Win32;
 * use FindFirstFileW/FindNextFileW/FindClose over an opaque heap struct.
 *
 * A find record already carries everything host_fs_meta wants, so a read
 * costs no extra call here at all.
 */

#include "host/api/dir.h"
#include "core/str/oem.h"
#include "core/str/path.h"
#include "host/windows/win.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* The same conversion host/windows/fs.c does; a find record reports in the
 * same FILETIME the rest of Win32 does. */
static time_t filetime_to_time_t(const FILETIME *ft)
{
    ULARGE_INTEGER ull;
    ull.LowPart = ft->dwLowDateTime;
    ull.HighPart = ft->dwHighDateTime;
    if (ull.QuadPart < 116444736000000000ULL)
        return (time_t)0;
    return (time_t)((ull.QuadPart / 10000000ULL) - 11644473600ULL);
}

struct win_dir
{
    HANDLE h;
    WIN32_FIND_DATAW fd;
    bool first; /* FindFirstFileW already yielded the first entry */
    bool alive;
    wchar_t pattern[WIN_WPATH_MAX];
};

void *host_dir_open(const char *path)
{
    char native[HOST_MAX_PATH];
    wchar_t base[WIN_WPATH_MAX];
    if (!path_to_native(path, native, sizeof native))
        return NULL;
    if (oem_to_wide(native, (uint16_t *)base, WIN_WPATH_MAX) <= 0)
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

int host_dir_read(void *opaque, char *name, size_t namesz, struct host_fs_meta *meta)
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
    DWORD a = d->fd.dwFileAttributes;
    meta->is_dir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    meta->is_readonly = (a & FILE_ATTRIBUTE_READONLY) != 0;
    meta->is_hidden = (a & FILE_ATTRIBUTE_HIDDEN) != 0;
    meta->size = ((uint64_t)d->fd.nFileSizeHigh << 32) | d->fd.nFileSizeLow;
    meta->mtime = filetime_to_time_t(&d->fd.ftLastWriteTime);
    meta->crtime = filetime_to_time_t(&d->fd.ftCreationTime);
    oem_from_wide((const uint16_t *)d->fd.cFileName, name, namesz);
    return 1;
}

void host_dir_rewind(void *opaque)
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

void host_dir_close(void *opaque)
{
    struct win_dir *d = (struct win_dir *)opaque;
    if (!d)
        return;
    if (d->alive && d->h != INVALID_HANDLE_VALUE)
        FindClose(d->h);
    free(d);
}
