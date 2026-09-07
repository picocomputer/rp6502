/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Files on a Win32 filesystem, as osal/fs.h asks for them.
 *
 * Two transports, chosen at build time as osal/posix chooses between fs_aio.c
 * and fs_sync.c -- in one file rather than two, because the overlapped flag
 * belongs to the handle and not to the transfer. Overlapped is the default:
 * the transfer is started, the dispatcher is told STD_PENDING, and a later
 * dispatch reaps it. RP6502_FS_SYNC completes it before it answers.
 *
 * The choice is 6502-visible. A read that completes in a different number of
 * frames diverges the machine at the load point, so a savestate blob is
 * portable only between roots that took the same transport. The libretro core
 * takes the sync arm to make the frames a read costs its machine's own rather
 * than the kernel's.
 *
 * Neither arm reads the handle's file pointer: an overlapped handle has none,
 * and the sync arm transfers at an explicit offset. So a descriptor is an
 * index into a table that carries the offset. Under overlapped one transfer
 * is in flight at a time (the dispatcher is single-op, and re-dispatches
 * until it retires) and fs_std_close and fs_std_settle reap it before the
 * handle or the memory goes away. Under sync there is never one to reap.
 *
 * Paths cross in the guest's OEM code page and are otherwise Win32's own;
 * path_to_wide() (osal/windows/dir.h) changes the code page before every ...W
 * call. Failures are reported with osal/windows/errmap.h, straight from
 * GetLastError.
 */

#include "osal/fs.h"
#include "core/str/oem.h"
#include "osal/os.h"
#include "osal/dir.h"
#include "osal/windows/dir.h"
#include "osal/windows/errmap.h"
#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h> /* SEEK_SET/CUR/END */
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <time.h>
#include <windows.h>

#ifdef RP6502_FS_SYNC
#define WIN_OPEN_FLAGS FILE_ATTRIBUTE_NORMAL
#else
#define WIN_OPEN_FLAGS (FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED)
#endif

/* Neither transport keeps its place in the handle, so a descriptor is an
 * index into this table and the offset is ours to track. 16 open files + 16
 * ROM windows = 32 concurrent; 64 leaves headroom for tests. One more beyond
 * the table is the ROM loader's alone -- fs_std_open never counts that high,
 * so a program can neither name it nor be handed it. */
#define WIN_MAX_FILES 64
#define WIN_FILE_ROM WIN_MAX_FILES
static struct win_file
{
    bool used;
    bool writable; /* a seek past the end extends this file rather than stopping */
    HANDLE h;
    int64_t pos;
    /* What a savestate needs to find this file again in another session: the
     * path made absolute after the open succeeded, so a create resolves too
     * and a later chdir cannot move it, and the access it was opened with.
     * No pool of its own as POSIX needs -- a descriptor here is an index into
     * this table, so the record can live in the row it describes. */
    uint8_t flags;
    char path[API_PATH_MAX + 1];
} win_files[WIN_MAX_FILES + 1];

static struct win_file *win_fil(int fd)
{
    if (fd < 0 || fd > WIN_FILE_ROM || !win_files[fd].used)
        return NULL;
    return &win_files[fd];
}

#ifndef RP6502_FS_SYNC
/* The single in-flight transfer (guest dispatcher is single-op, so only one exists at a
 * time). fd < 0 = idle; g_xfer_event is its manual-reset completion event. A reader from
 * outside that dispatch -- the dropped-file screen, which runs with a program still
 * going -- is refused, not served this transfer. */
static struct
{
    OVERLAPPED ov;
    int fd;
} g_xfer = {.fd = -1};
static HANDLE g_xfer_event;
#endif

/* ---- The std driver ------------------------------------------------------ */

bool fs_std_handles(const char *path)
{
    (void)path;
    return true; /* catch-all, registered last */
}

static HANDLE win_open_handle(const char *path, uint8_t flags, api_errno *err)
{
    wchar_t *w = path_to_wide(path, err);
    if (!w)
        return INVALID_HANDLE_VALUE;
    bool wr = (flags & FS_WR) != 0;
    DWORD access = wr ? ((flags & FS_RD) ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_WRITE)
                      : GENERIC_READ;
    DWORD disp;
    if ((flags & FS_CREAT) && (flags & FS_EXCL))
        disp = CREATE_NEW;
    else if ((flags & FS_CREAT) && (flags & FS_TRUNC) && wr)
        disp = CREATE_ALWAYS;
    else if (flags & FS_CREAT)
        disp = OPEN_ALWAYS;
    else if ((flags & FS_TRUNC) && wr)
        disp = TRUNCATE_EXISTING;
    else
        disp = OPEN_EXISTING;

    HANDLE h = CreateFileW(w, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, disp, WIN_OPEN_FLAGS, NULL);
    if (h == INVALID_HANDLE_VALUE)
        *err = win_last_error_to_api(); /* before the free, which may clobber it */
    free(w);
    return h;
}

int fs_std_open(const char *path, uint8_t flags, api_errno *err)
{
    HANDLE h = win_open_handle(path, flags, err);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    int fd = 0;
    for (; fd < WIN_MAX_FILES; fd++)
        if (!win_files[fd].used)
            break;
    if (fd == WIN_MAX_FILES)
    {
        CloseHandle(h);
        *err = API_EMFILE;
        return -1;
    }
    win_files[fd] = (struct win_file){.used = true, .h = h, .pos = 0,
                                      .writable = (flags & FS_WR) != 0,
                                      .flags = (uint8_t)(flags & (FS_RD | FS_WR))};
    char *abs = os_dir_realpath(path);
    const char *keep = abs ? abs : path;
    if (strlen(keep) <= API_PATH_MAX)
        strcpy(win_files[fd].path, keep);
    free(abs);
    if (flags & FS_APPEND) /* a one-time seek to the end, after any TRUNC */
    {
        LARGE_INTEGER sz;
        if (!GetFileSizeEx(h, &sz))
        {
            *err = win_last_error_to_api();
            win_files[fd].used = false;
            CloseHandle(h);
            return -1;
        }
        win_files[fd].pos = (int64_t)sz.QuadPart;
    }
    return fd;
}

/* The position is this table's rather than the handle's: neither transport
 * reads the handle's own file pointer, so f->pos is the only one there is. */
bool fs_std_ident(int desc, sst_cursor_t *c)
{
    struct win_file *f = win_fil(desc);
    if (!f || !f->path[0] || f->pos < 0 || f->pos > INT32_MAX)
        return false;
    sst_put_str(c, f->path, FS_PATH_SLOT);
    sst_put_u8(c, f->flags);
    sst_put_i32(c, (int32_t)f->pos);
    return sst_ok(c);
}

int fs_std_reopen(sst_cursor_t *c, api_errno *err)
{
    char path[FS_PATH_SLOT];
    sst_get_str(c, path, FS_PATH_SLOT);
    uint8_t flags = sst_get_u8(c);
    int32_t pos = sst_get_i32(c);
    if (!sst_ok(c) || pos < 0)
    {
        *err = API_EINVAL;
        return -1;
    }
    /* The access bits only. CREAT, EXCL, TRUNC and APPEND are one-time acts
     * the opening program already had, and re-firing one would make or empty
     * the very file this is trying to find again. */
    int fd = fs_std_open(path, flags & (FS_RD | FS_WR), err);
    if (fd < 0)
        return -1;
    win_files[fd].pos = pos;
    return fd;
}

int fs_rom_open(const char *path, uint8_t flags, api_errno *err)
{
    if (flags != FS_RD)
    {
        *err = (flags == (FS_WR | FS_CREAT | FS_EXCL)) ? API_EACCES : API_EINVAL;
        return -1;
    }
    HANDLE h = win_open_handle(path, FS_RD, err);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    win_files[WIN_FILE_ROM] =
        (struct win_file){.used = true, .h = h, .pos = 0, .writable = false};
    return WIN_FILE_ROM;
}

bool fs_rom_remove(const char *name, api_errno *err)
{
    (void)name;
    *err = API_EACCES; /* installs are references; there is nothing to delete */
    return false;
}

void fs_std_settle(void)
{
#ifndef RP6502_FS_SYNC
    if (g_xfer.fd < 0)
        return;
    struct win_file *f = win_fil(g_xfer.fd);
    if (f)
    {
        /* The reap fs_std_close does, without the close and without the
         * f->pos advance a completed transfer would make. */
        DWORD bytes;
        CancelIoEx(f->h, &g_xfer.ov);
        GetOverlappedResult(f->h, &g_xfer.ov, &bytes, TRUE);
    }
    g_xfer.fd = -1;
#endif
}

std_rw_result fs_std_close(int desc, api_errno *err)
{
    int fd = desc;
    struct win_file *f = win_fil(fd);
    if (!f)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
#ifndef RP6502_FS_SYNC
    if (g_xfer.fd == fd) /* reap the in-flight transfer before the handle goes away */
    {
        DWORD bytes;
        CancelIoEx(f->h, &g_xfer.ov);
        GetOverlappedResult(f->h, &g_xfer.ov, &bytes, TRUE);
        g_xfer.fd = -1;
    }
#endif
    BOOL ok = CloseHandle(f->h);
    f->used = false;
    if (!ok)
    {
        *err = win_last_error_to_api();
        return STD_ERROR;
    }
    return STD_OK;
}

static std_rw_result xfer_step(int fd, void *buf, uint32_t count, uint32_t *got, bool is_write,
                               api_errno *err)
{
    *got = 0;
    struct win_file *f = win_fil(fd);
    if (!f)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
#ifdef RP6502_FS_SYNC
    /* A synchronous handle given an OVERLAPPED transfers at that offset and
     * does not return until it is done. */
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)((uint64_t)f->pos & 0xFFFFFFFFu);
    ov.OffsetHigh = (DWORD)((uint64_t)f->pos >> 32);
    DWORD bytes = 0;
    BOOL ok = is_write ? WriteFile(f->h, buf, count, &bytes, &ov)
                       : ReadFile(f->h, buf, count, &bytes, &ov);
    if (!ok)
    {
        DWORD e = GetLastError();
        if (e == ERROR_HANDLE_EOF) /* read at/after EOF: done, 0 bytes */
            return STD_OK;
        *err = win_error_to_api(e);
        return STD_ERROR;
    }
    f->pos += bytes; /* the kernel moved its own pointer, not the one we keep */
    *got = (uint32_t)bytes;
    return STD_OK;
#else
    if (g_xfer.fd >= 0 && g_xfer.fd != fd)
    {
        /* The slot holds someone else's transfer. Reaping it here would hand
         * this caller that one's byte count and leave its buffer unwritten. */
        *err = API_EBUSY;
        return STD_ERROR;
    }
    if (g_xfer.fd < 0)
    {
        if (!g_xfer_event && !(g_xfer_event = CreateEventW(NULL, TRUE, FALSE, NULL)))
        {
            *err = win_last_error_to_api();
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
                *err = win_error_to_api(e);
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
        *err = win_error_to_api(e);
        return STD_ERROR;
    }
    g_xfer.fd = -1;
    f->pos += bytes; /* the overlapped handle didn't move; advance our tracked offset */
    *got = (uint32_t)bytes;
    return STD_OK;
#endif
}

std_rw_result fs_std_read(int desc, char *buf, uint32_t count, uint32_t *got, api_errno *err)
{
    return xfer_step(desc, buf, count, got, false, err);
}

std_rw_result fs_std_write(int desc, const char *buf, uint32_t count, uint32_t *put, api_errno *err)
{
    return xfer_step(desc, (void *)buf, count, put, true, err);
}

static int64_t win_size_of(struct win_file *f, api_errno *err)
{
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f->h, &sz))
    {
        *err = win_last_error_to_api();
        return -1;
    }
    return (int64_t)sz.QuadPart;
}

/* The position is ours to keep rather than the handle's -- but the file's
 * length is still the kernel's, and extending it is a real call that can fail
 * on a full volume. */
int fs_std_lseek(int desc, int8_t whence, int32_t off, int32_t *pos, api_errno *err)
{
    struct win_file *f = win_fil(desc);
    if (!f)
    {
        *err = API_EBADF;
        return -1;
    }
    int64_t base;
    if (whence == SEEK_SET)
        base = 0;
    else if (whence == SEEK_CUR)
        base = f->pos;
    else if (whence == SEEK_END)
    {
        base = win_size_of(f, err);
        if (base < 0)
            return -1;
    }
    else
    {
        *err = API_EINVAL;
        return -1;
    }
    /* The position comes back as a signed 32-bit value (0xFFFFFFFF is the
     * error sentinel), so a target past 2GB-1 is refused before the pointer
     * moves rather than landing somewhere unreportable. */
    int64_t target = base + off;
    if (target < 0)
    {
        *err = API_EINVAL;
        return -1;
    }
    if (target > 0x7FFFFFFF)
    {
        *err = API_ERANGE;
        return -1;
    }
    int64_t size = win_size_of(f, err);
    if (size < 0)
        return -1;
    if (target > size)
    {
        if (!f->writable)
            target = size; /* read-only: stop at the end */
        else
        {
            FILE_END_OF_FILE_INFO eof = {.EndOfFile = {.QuadPart = (LONGLONG)target}};
            if (!SetFileInformationByHandle(f->h, FileEndOfFileInfo, &eof, sizeof eof))
            {
                *err = win_last_error_to_api();
                return -1; /* no room: the pointer has not moved */
            }
        }
    }
    f->pos = target;
    *pos = (int32_t)target;
    return 0;
}

std_rw_result fs_std_sync(int desc, api_errno *err)
{
    struct win_file *f = win_fil(desc);
    if (!f)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    /* Nothing was written, so there is nothing to push to the medium -- and
     * FlushFileBuffers refuses a handle that has no write access at all,
     * which is what a read-only descriptor is. */
    if (!f->writable)
        return STD_OK;
    if (!FlushFileBuffers(f->h))
    {
        *err = win_last_error_to_api();
        return STD_ERROR;
    }
    return STD_OK;
}
