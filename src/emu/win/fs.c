/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Windows filesystem primitives (emu/plat.h fs_*), the Win32 counterpart of
 * posix/fs.c.
 *
 * Paths cross the seam in the guest's OEM code page. Convert to UTF-16 with
 * oem_to_wide() (emu/api/oem.h) before every …W call, and returned names/paths back
 * with oem_from_wide(). Fallible calls set errno and return false so the
 * msc_errno_to_api_errno funnel in api/hostfs.c works unchanged.
 *
 * Semantic decisions vs. POSIX (do NOT silently diverge):
 *   - is_hidden reads FILE_ATTRIBUTE_HIDDEN, not a leading-dot name.
 *   - crtime is the real ftCreationTime (POSIX has none; it reports change time).
 *   - fs_rename MUST replace an existing target (MOVEFILE_REPLACE_EXISTING).
 *   - fs_remove MUST also delete an empty directory (POSIX remove() does).
 *   - open the CRT in binary mode (_O_BINARY).
 */

#include "emu/plat.h"
#include "emu/api/oem.h"
#include "emu/win/win.h"
#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>

static bool path_to_wide(const char *path, wchar_t *w, int wcount)
{
    if (oem_to_wide(path, (uint16_t *)w, wcount) < 0 || !w[0])
    {
        errno = EINVAL;
        return false;
    }
    return true;
}

static time_t filetime_to_time_t(const FILETIME *ft)
{
    ULARGE_INTEGER ull;
    ull.LowPart = ft->dwLowDateTime;
    ull.HighPart = ft->dwHighDateTime;
    if (ull.QuadPart < 116444736000000000ULL)
        return (time_t)0;
    return (time_t)((ull.QuadPart / 10000000ULL) - 11644473600ULL);
}

static void time_t_to_filetime(time_t t, FILETIME *ft)
{
    ULARGE_INTEGER ull;
    ull.QuadPart = ((uint64_t)t + 11644473600ULL) * 10000000ULL;
    ft->dwLowDateTime = ull.LowPart;
    ft->dwHighDateTime = (DWORD)ull.HighPart;
}

bool fs_stat(const char *path, struct fs_meta *out)
{
    wchar_t w[WIN_WPATH_MAX];
    if (!path_to_wide(path, w, WIN_WPATH_MAX))
        return false;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(w, GetFileExInfoStandard, &fad))
    {
        win_set_errno(GetLastError());
        return false;
    }
    out->is_dir = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    out->is_readonly = (fad.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
    out->is_hidden = (fad.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
    out->size = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    out->mtime = filetime_to_time_t(&fad.ftLastWriteTime);
    out->crtime = filetime_to_time_t(&fad.ftCreationTime);
    return true;
}

bool fs_freespace(const char *path, uint64_t *total_bytes, uint64_t *avail_bytes)
{
    wchar_t w[WIN_WPATH_MAX];
    if (!path_to_wide(path, w, WIN_WPATH_MAX))
        return false;
    /* Prefer the parent directory when path names a file. */
    wchar_t dir[WIN_WPATH_MAX];
    wcsncpy(dir, w, WIN_WPATH_MAX - 1);
    dir[WIN_WPATH_MAX - 1] = 0;
    wchar_t *slash = wcsrchr(dir, L'\\');
    wchar_t *slash2 = wcsrchr(dir, L'/');
    if (slash2 && (!slash || slash2 > slash))
        slash = slash2;
    if (slash && slash != dir && !(slash == dir + 2 && dir[1] == L':'))
        *slash = 0;
    ULARGE_INTEGER avail, total;
    if (!GetDiskFreeSpaceExW(dir, &avail, &total, NULL))
    {
        win_set_errno(GetLastError());
        return false;
    }
    *total_bytes = total.QuadPart;
    *avail_bytes = avail.QuadPart;
    return true;
}

bool fs_set_readonly(const char *path, bool readonly)
{
    wchar_t w[WIN_WPATH_MAX];
    if (!path_to_wide(path, w, WIN_WPATH_MAX))
        return false;
    DWORD a = GetFileAttributesW(w);
    if (a == INVALID_FILE_ATTRIBUTES)
    {
        win_set_errno(GetLastError());
        return false;
    }
    if (readonly)
        a |= FILE_ATTRIBUTE_READONLY;
    else
        a &= ~FILE_ATTRIBUTE_READONLY;
    if (!SetFileAttributesW(w, a))
    {
        win_set_errno(GetLastError());
        return false;
    }
    return true;
}

bool fs_set_mtime(const char *path, time_t mtime)
{
    wchar_t w[WIN_WPATH_MAX];
    if (!path_to_wide(path, w, WIN_WPATH_MAX))
        return false;
    HANDLE h = CreateFileW(w, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        win_set_errno(GetLastError());
        return false;
    }
    FILETIME ft;
    time_t_to_filetime(mtime, &ft);
    BOOL ok = SetFileTime(h, NULL, NULL, &ft);
    DWORD err = GetLastError();
    CloseHandle(h);
    if (!ok)
    {
        win_set_errno(err);
        return false;
    }
    return true;
}

bool fs_mkdir(const char *path)
{
    wchar_t w[WIN_WPATH_MAX];
    if (!path_to_wide(path, w, WIN_WPATH_MAX))
        return false;
    if (_wmkdir(w) != 0)
        return false;
    return true;
}

bool fs_chdir(const char *path)
{
    wchar_t w[WIN_WPATH_MAX];
    if (!path_to_wide(path, w, WIN_WPATH_MAX))
        return false;
    return _wchdir(w) == 0;
}

bool fs_getcwd(char *buf, size_t sz)
{
    wchar_t *w = _wgetcwd(NULL, 0);
    if (!w)
        return false;
    oem_from_wide((const uint16_t *)w, buf, sz);
    free(w);
    win_to_slash(buf);
    return true;
}

bool fs_realpath(const char *path, char *out, size_t outsz)
{
    wchar_t wpath[WIN_WPATH_MAX], wfull[WIN_WPATH_MAX];
    if (!path_to_wide(path, wpath, WIN_WPATH_MAX))
        return false;
    DWORD n = GetFullPathNameW(wpath, WIN_WPATH_MAX, wfull, NULL);
    if (n == 0 || n >= WIN_WPATH_MAX)
    {
        win_set_errno(n ? ERROR_FILENAME_EXCED_RANGE : GetLastError());
        return false;
    }
    oem_from_wide((const uint16_t *)wfull, out, outsz);
    win_to_slash(out);
    return true;
}

bool fs_rename(const char *oldp, const char *newp)
{
    wchar_t wo[WIN_WPATH_MAX], wn[WIN_WPATH_MAX];
    if (!path_to_wide(oldp, wo, WIN_WPATH_MAX) || !path_to_wide(newp, wn, WIN_WPATH_MAX))
        return false;
    if (!MoveFileExW(wo, wn, MOVEFILE_REPLACE_EXISTING))
    {
        win_set_errno(GetLastError());
        return false;
    }
    return true;
}

bool fs_remove(const char *path)
{
    wchar_t w[WIN_WPATH_MAX];
    if (!path_to_wide(path, w, WIN_WPATH_MAX))
        return false;
    if (_wunlink(w) == 0)
        return true;
    if (errno == EACCES || errno == EPERM)
    {
        if (_wrmdir(w) == 0)
            return true;
    }
    return false;
}

int fs_open(const char *path, int flags, int mode)
{
    wchar_t w[WIN_WPATH_MAX];
    if (!path_to_wide(path, w, WIN_WPATH_MAX))
        return -1;
    int oflags = flags | _O_BINARY;
    return _wopen(w, oflags, mode);
}

int fs_close(int fd)
{
    return _close(fd);
}

fs_ssize_t fs_read(int fd, void *buf, size_t n)
{
    unsigned int chunk = n > 0x7fffffffU ? 0x7fffffffU : (unsigned int)n;
    int r = _read(fd, buf, chunk);
    return (fs_ssize_t)r;
}

fs_ssize_t fs_write(int fd, const void *buf, size_t n)
{
    unsigned int chunk = n > 0x7fffffffU ? 0x7fffffffU : (unsigned int)n;
    int r = _write(fd, buf, chunk);
    return (fs_ssize_t)r;
}

int64_t fs_lseek(int fd, int64_t off, int whence)
{
    return _lseeki64(fd, off, whence);
}

int fs_ftruncate(int fd, int64_t length)
{
    return _chsize_s(fd, length);
}

fs_ssize_t fs_pread(int fd, void *buf, size_t n, int64_t off)
{
    int64_t cur = _lseeki64(fd, 0, SEEK_CUR);
    if (cur < 0)
        return -1;
    if (_lseeki64(fd, off, SEEK_SET) < 0)
        return -1;
    fs_ssize_t r = fs_read(fd, buf, n);
    int save = errno;
    if (_lseeki64(fd, cur, SEEK_SET) < 0 && r >= 0)
    {
        /* Prefer the seek-restore failure if the read succeeded. */
        return -1;
    }
    errno = save;
    return r;
}
