/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Windows filesystem primitives (emu/plat.h fs_*), the Win32 counterpart of
 * posix/fs.c.
 *
 * Paths cross the seam in the guest's OEM code page. Convert to UTF-16 with
 * oem_to_wide() (api/oem.h) before every …W call, and returned names/paths back
 * with oem_from_wide(). Fallible calls set errno and return false so the
 * msc_errno_to_api_errno funnel in api/hostfs.c works unchanged.
 *
 * Semantic decisions vs. POSIX (do NOT silently diverge):
 *   - is_hidden reads FILE_ATTRIBUTE_HIDDEN, not a leading-dot name.
 *   - crtime is the real ftCreationTime (POSIX has none; it reports change time).
 *   - fs_rename MUST replace an existing target (MOVEFILE_REPLACE_EXISTING).
 *   - fs_remove MUST also delete an empty directory (POSIX remove() does).
 *   - byte I/O uses overlapped HANDLEs (FILE_FLAG_OVERLAPPED) with a tracked offset, so
 *     fs_read/fs_write are the non-blocking transfer; fs_open returns an index into
 *     win_files (an overlapped handle has no implicit file pointer, so we carry our own).
 */

#include "emu/plat.h"
#include "api/oem.h"
#include "emu/host/win/win.h"
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

/* An overlapped handle has no implicit file pointer, so fs_open returns an index into
 * this table and we track the offset ourselves. 16 host files + 16 ROM windows = 32
 * concurrent; 64 leaves headroom for tests. */
#define WIN_MAX_FILES 64
static struct win_file
{
    bool used;
    HANDLE h;
    int64_t pos;
} win_files[WIN_MAX_FILES];

static struct win_file *win_fil(int fd)
{
    if (fd < 0 || fd >= WIN_MAX_FILES || !win_files[fd].used)
        return NULL;
    return &win_files[fd];
}

/* The single in-flight transfer (guest dispatcher is single-op, so only one exists at a
 * time). fd < 0 = idle; g_xfer_event is its manual-reset completion event. */
static struct
{
    OVERLAPPED ov;
    int fd;
} g_xfer = {.fd = -1};
static HANDLE g_xfer_event;

int fs_open(const char *path, int flags, int mode)
{
    (void)mode; /* Windows takes permissions from the file; msc opens writable */
    wchar_t w[WIN_WPATH_MAX];
    if (!path_to_wide(path, w, WIN_WPATH_MAX))
        return -1;

    DWORD access;
    switch (flags & (O_WRONLY | O_RDWR))
    {
    case O_WRONLY: access = GENERIC_WRITE; break;
    case O_RDWR: access = GENERIC_READ | GENERIC_WRITE; break;
    default: access = GENERIC_READ; break; /* O_RDONLY */
    }
    DWORD disp;
    if ((flags & O_CREAT) && (flags & O_EXCL))
        disp = CREATE_NEW;
    else if ((flags & O_CREAT) && (flags & O_TRUNC))
        disp = CREATE_ALWAYS;
    else if (flags & O_CREAT)
        disp = OPEN_ALWAYS;
    else if (flags & O_TRUNC)
        disp = TRUNCATE_EXISTING;
    else
        disp = OPEN_EXISTING;

    HANDLE h = CreateFileW(w, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, disp, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        win_set_errno(GetLastError());
        return -1;
    }
    int fd = 0;
    for (; fd < WIN_MAX_FILES; fd++)
        if (!win_files[fd].used)
            break;
    if (fd == WIN_MAX_FILES)
    {
        CloseHandle(h);
        errno = EMFILE;
        return -1;
    }
    win_files[fd] = (struct win_file){.used = true, .h = h, .pos = 0};
    return fd;
}

FILE *fs_fopen_rd(const char *path)
{
    wchar_t w[WIN_WPATH_MAX];
    if (!path_to_wide(path, w, WIN_WPATH_MAX))
        return NULL;
    return _wfopen(w, L"rb");
}

int fs_close(int fd)
{
    struct win_file *f = win_fil(fd);
    if (!f)
    {
        errno = EBADF;
        return -1;
    }
    if (g_xfer.fd == fd) /* reap the in-flight transfer before the handle goes away */
    {
        DWORD bytes;
        CancelIoEx(f->h, &g_xfer.ov);
        GetOverlappedResult(f->h, &g_xfer.ov, &bytes, TRUE);
        g_xfer.fd = -1;
    }
    BOOL ok = CloseHandle(f->h);
    f->used = false;
    if (!ok)
    {
        win_set_errno(GetLastError());
        return -1;
    }
    return 0;
}

static std_rw_result xfer_step(int fd, void *buf, uint32_t count, uint32_t *got, bool is_write)
{
    *got = 0;
    struct win_file *f = win_fil(fd);
    if (!f)
    {
        errno = EBADF;
        return STD_ERROR;
    }
    if (g_xfer.fd < 0)
    {
        if (!g_xfer_event && !(g_xfer_event = CreateEventW(NULL, TRUE, FALSE, NULL)))
        {
            win_set_errno(GetLastError());
            return STD_ERROR;
        }
        ResetEvent(g_xfer_event);
        memset(&g_xfer.ov, 0, sizeof g_xfer.ov);
        g_xfer.ov.hEvent = g_xfer_event;
        g_xfer.ov.Offset = (DWORD)((uint64_t)f->pos & 0xFFFFFFFFu);
        g_xfer.ov.OffsetHigh = (DWORD)((uint64_t)f->pos >> 32);
        BOOL ok = is_write ? WriteFile(f->h, buf, count, NULL, &g_xfer.ov)
                           : ReadFile(f->h, buf, count, NULL, &g_xfer.ov);
        if (!ok)
        {
            DWORD e = GetLastError();
            if (e == ERROR_HANDLE_EOF) /* read at/after EOF: done, 0 bytes */
                return STD_OK;
            if (e != ERROR_IO_PENDING)
            {
                win_set_errno(e);
                return STD_ERROR;
            }
        }
        g_xfer.fd = fd; /* completed synchronously or queued: reap on the next dispatch */
        return STD_PENDING;
    }
    DWORD bytes = 0;
    if (!GetOverlappedResult(f->h, &g_xfer.ov, &bytes, FALSE))
    {
        DWORD e = GetLastError();
        if (e == ERROR_IO_INCOMPLETE)
            return STD_PENDING;
        g_xfer.fd = -1;
        if (e == ERROR_HANDLE_EOF) /* completed at EOF: 0 bytes */
            return STD_OK;
        win_set_errno(e);
        return STD_ERROR;
    }
    g_xfer.fd = -1;
    f->pos += bytes; /* the overlapped handle didn't move; advance our tracked offset */
    *got = (uint32_t)bytes;
    return STD_OK;
}

std_rw_result fs_read(int fd, char *buf, uint32_t count, uint32_t *got)
{
    return xfer_step(fd, buf, count, got, false);
}

std_rw_result fs_write(int fd, const char *buf, uint32_t count, uint32_t *put)
{
    return xfer_step(fd, (void *)buf, count, put, true);
}

int64_t fs_lseek(int fd, int64_t off, int whence)
{
    struct win_file *f = win_fil(fd);
    if (!f)
    {
        errno = EBADF;
        return -1;
    }
    int64_t base;
    if (whence == SEEK_SET)
        base = 0;
    else if (whence == SEEK_CUR)
        base = f->pos;
    else if (whence == SEEK_END)
    {
        LARGE_INTEGER sz;
        if (!GetFileSizeEx(f->h, &sz))
        {
            win_set_errno(GetLastError());
            return -1;
        }
        base = sz.QuadPart;
    }
    else
    {
        errno = EINVAL;
        return -1;
    }
    int64_t np = base + off;
    if (np < 0)
    {
        errno = EINVAL;
        return -1;
    }
    f->pos = np;
    return np;
}

int fs_ftruncate(int fd, int64_t length)
{
    struct win_file *f = win_fil(fd);
    if (!f)
    {
        errno = EBADF;
        return -1;
    }
    FILE_END_OF_FILE_INFO eof = {.EndOfFile = {.QuadPart = length}};
    if (!SetFileInformationByHandle(f->h, FileEndOfFileInfo, &eof, sizeof eof))
    {
        win_set_errno(GetLastError());
        return -1;
    }
    return 0;
}

void fs_sync(void) {} /* a real host filesystem is already durable */
