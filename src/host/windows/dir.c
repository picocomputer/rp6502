/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This machine's drive, as core/api/dir.h asks for it: the directory syscalls
 * answered over Win32. The counterpart of host/posix/dir.c.
 *
 * Windows keeps what the 6502 asks for. FILE_ATTRIBUTE_READONLY, _HIDDEN,
 * _SYSTEM, _DIRECTORY and _ARCHIVE are the FAT attribute bits, with the same
 * values FAT gave them, and FileTimeToDosDateTime is the FAT date and time --
 * so a FILINFO is read off a find record rather than reconstructed, and none
 * of it is a guess. A find carries all of it, so a read costs no extra call.
 *
 * There is no opendir/readdir on Win32: FindFirstFileW/FindNextFileW/FindClose
 * over an opaque heap struct. Nothing collides with ff.h here, so unlike the
 * POSIX side this is one file.
 *
 * Paths cross spelled the way the 6502 spells them and in its OEM code page.
 * The drive prefix comes off with path_to_native() and the code page with
 * oem_to_wide() (core/str/oem.h) before every ...W call; returned names go
 * back with oem_from_wide().
 */

#include "core/api/dir.h"
#include "core/str/oem.h"
#include "core/str/path.h"
#include "host/os.h"
#include "host/windows/errmap.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <windows.h>

#define DIR_NAME_MAX 256 /* an entry's name, not a path */

/* A path arrives spelled the way the 6502 spells it. This drive is one
 * directory of a real filesystem, so the drive prefix comes off here and what
 * is left is the native path -- and then the code page comes off too. */
static bool path_to_wide(const char *path, wchar_t *w, int wcount, api_errno *err)
{
    char native[HOST_MAX_PATH];
    if (!path_to_native(path, native, sizeof native) ||
        oem_to_wide(native, (uint16_t *)w, wcount) < 0)
    {
        *err = API_EINVAL;
        return false;
    }
    return true;
}

/* Whatever Win32 last complained about, in the API's words. */
static bool win_ok(BOOL ok, api_errno *err)
{
    if (!ok)
        *err = win_last_error_to_api();
    return ok != FALSE;
}

/* ---- FILINFO, straight off what Win32 keeps ------------------------------ */

/* The FAT attribute bits the 6502 sees, which are the same bits Win32 uses --
 * masked so nothing Windows-only (COMPRESSED, REPARSE_POINT, ...) leaks into
 * a field a program reads as FAT's. */
#define FS_AM_MASK 0x37 /* RDO|HID|SYS|DIR|ARC */

static void info_from_find(FILINFO *fno, const WIN32_FIND_DATAW *fd, const char *name)
{
    snprintf(fno->fname, sizeof(fno->fname), "%s", name);
    fno->altname[0] = 0; /* the 8.3 name Win32 offers is not asked for here */
    uint64_t size = ((uint64_t)fd->nFileSizeHigh << 32) | fd->nFileSizeLow;
    fno->fsize = size > 0xFFFFFFFF ? 0xFFFFFFFF : (FSIZE_t)size;
    fno->fattrib = (uint8_t)(fd->dwFileAttributes & FS_AM_MASK);
    /* A find reports UTC; FAT records local time, which is what the API
     * carries, so each stamp goes through the local conversion on the way. */
    FILETIME lft;
    WORD d = 0, t = 0;
    if (FileTimeToLocalFileTime(&fd->ftLastWriteTime, &lft))
        FileTimeToDosDateTime(&lft, &d, &t);
    fno->fdate = d;
    fno->ftime = t;
    d = t = 0;
    if (FileTimeToLocalFileTime(&fd->ftCreationTime, &lft))
        FileTimeToDosDateTime(&lft, &d, &t);
    fno->crdate = d;
    fno->crtime = t;
}

/* ---- The drive, as core/api/dir.c asks for it ---------------------------- */

struct win_dir
{
    bool used;
    HANDLE h;
    WIN32_FIND_DATAW fd;
    bool first; /* FindFirstFileW already yielded the first entry */
    bool alive;
    wchar_t pattern[WIN_WPATH_MAX];
};
static struct win_dir dirs[DIR_MAX_OPEN];

static bool drive_validate(int des, api_errno *err)
{
    if (des < 0 || des >= DIR_MAX_OPEN)
    {
        *err = API_EINVAL;
        return false;
    }
    if (!dirs[des].used)
    {
        *err = API_EBADF;
        return false;
    }
    return true;
}

static bool drive_stat(const char *path, FILINFO *fno, api_errno *err)
{
    wchar_t w[WIN_WPATH_MAX];
    if (!path_to_wide(path, w, WIN_WPATH_MAX, err))
        return false;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!win_ok(GetFileAttributesExW(w, GetFileExInfoStandard, &fad), err))
        return false;
    /* The two records agree on every field this reads. */
    WIN32_FIND_DATAW fd;
    memset(&fd, 0, sizeof fd);
    fd.dwFileAttributes = fad.dwFileAttributes;
    fd.ftLastWriteTime = fad.ftLastWriteTime;
    fd.ftCreationTime = fad.ftCreationTime;
    fd.nFileSizeHigh = fad.nFileSizeHigh;
    fd.nFileSizeLow = fad.nFileSizeLow;
    /* stat names a single entry; report its basename, not the whole path. */
    info_from_find(fno, &fd, path_basename(path));
    return true;
}

static bool drive_opendir(const char *path, int *des, api_errno *err)
{
    int i = 0;
    for (; i < DIR_MAX_OPEN; i++)
        if (!dirs[i].used)
            break;
    if (i == DIR_MAX_OPEN)
    {
        *err = API_EMFILE;
        return false;
    }
    struct win_dir *d = &dirs[i];
    wchar_t base[WIN_WPATH_MAX];
    if (!path_to_wide(path, base, WIN_WPATH_MAX, err))
        return false;
    if (!base[0]) /* a directory of no name is the working directory */
        base[0] = L'.', base[1] = 0;
    size_t n = wcslen(base);
    while (n > 0 && (base[n - 1] == L'\\' || base[n - 1] == L'/'))
        base[--n] = 0;
    if (n + 3 >= WIN_WPATH_MAX)
    {
        *err = API_EINVAL;
        return false;
    }
    memcpy(d->pattern, base, (n + 1) * sizeof(wchar_t));
    d->pattern[n++] = L'\\';
    d->pattern[n++] = L'*';
    d->pattern[n] = 0;

    d->h = FindFirstFileW(d->pattern, &d->fd);
    if (!win_ok(d->h != INVALID_HANDLE_VALUE, err))
        return false;
    d->used = true;
    d->first = true;
    d->alive = true;
    *des = i;
    return true;
}

/* "." and ".." are not entries the 6502 sees. */
static bool drive_readdir(int des, FILINFO *fno, api_errno *err)
{
    struct win_dir *d = &dirs[des];
    if (!d->alive)
    {
        *err = API_EBADF;
        return false;
    }
    for (;;)
    {
        if (!d->first)
        {
            if (!FindNextFileW(d->h, &d->fd))
            {
                DWORD e = GetLastError();
                if (e == ERROR_NO_MORE_FILES)
                {
                    memset(fno, 0, sizeof(*fno)); /* fname[0]==0 signals EOF */
                    return true;
                }
                *err = win_error_to_api(e);
                return false;
            }
        }
        d->first = false;
        char name[DIR_NAME_MAX];
        oem_from_wide((const uint16_t *)d->fd.cFileName, name, sizeof name);
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        info_from_find(fno, &d->fd, name);
        return true;
    }
}

static bool drive_closedir(int des, api_errno *err)
{
    (void)err;
    struct win_dir *d = &dirs[des];
    if (d->alive && d->h != INVALID_HANDLE_VALUE)
        FindClose(d->h);
    d->used = false;
    d->alive = false;
    d->h = INVALID_HANDLE_VALUE;
    return true;
}

static bool drive_rewinddir(int des, api_errno *err)
{
    struct win_dir *d = &dirs[des];
    if (d->alive && d->h != INVALID_HANDLE_VALUE)
        FindClose(d->h);
    d->h = FindFirstFileW(d->pattern, &d->fd);
    if (!win_ok(d->h != INVALID_HANDLE_VALUE, err))
    {
        d->alive = false;
        return false;
    }
    d->first = true;
    d->alive = true;
    return true;
}

static bool drive_unlink(const char *path, api_errno *err)
{
    wchar_t w[WIN_WPATH_MAX];
    if (!path_to_wide(path, w, WIN_WPATH_MAX, err))
        return false;
    if (DeleteFileW(w))
        return true;
    DWORD e = GetLastError();
    /* One call for both, the way the API asks: a directory refuses DeleteFile
     * with an access complaint, and RemoveDirectory is what it wanted. */
    if (e == ERROR_ACCESS_DENIED && RemoveDirectoryW(w))
        return true;
    *err = win_error_to_api(e);
    return false;
}

static bool drive_rename(const char *oldname, const char *newname, api_errno *err)
{
    wchar_t wo[WIN_WPATH_MAX], wn[WIN_WPATH_MAX];
    if (!path_to_wide(oldname, wo, WIN_WPATH_MAX, err) ||
        !path_to_wide(newname, wn, WIN_WPATH_MAX, err))
        return false;
    return win_ok(MoveFileExW(wo, wn, MOVEFILE_REPLACE_EXISTING), err);
}

static bool drive_mkdir(const char *path, api_errno *err)
{
    wchar_t w[WIN_WPATH_MAX];
    if (!path_to_wide(path, w, WIN_WPATH_MAX, err))
        return false;
    return win_ok(CreateDirectoryW(w, NULL), err);
}

static bool drive_chdir(const char *path, api_errno *err)
{
    wchar_t w[WIN_WPATH_MAX];
    if (!path_to_wide(path, w, WIN_WPATH_MAX, err))
        return false;
    /* validates existence and dir-ness */
    return win_ok(SetCurrentDirectoryW(w), err);
}

/* The 6502 sees MSC0: (and the bare current drive); anything else is a
 * missing device. */
static bool drive_chdrive(const char *drive, api_errno *err)
{
    if (drive[0] != ':') /* the null drive (installs) is not a cwd-able drive */
    {
        char name[16];
        size_t i = 0;
        for (; drive[i] && drive[i] != ':' && i < sizeof(name) - 1; i++)
            name[i] = drive[i];
        name[i] = 0;
        if (name[0] == 0 || strcasecmp(name, "MSC0") == 0)
            return true;
    }
    *err = API_ENODEV;
    return false;
}

/* The attribute bits are Win32's own, so only the ones the API names are
 * touched and the rest of what Windows keeps is left alone. */
static bool drive_chmod(const char *path, uint8_t attr, uint8_t mask, api_errno *err)
{
    if (!(mask & FS_AM_MASK))
        return true;
    wchar_t w[WIN_WPATH_MAX];
    if (!path_to_wide(path, w, WIN_WPATH_MAX, err))
        return false;
    DWORD a = GetFileAttributesW(w);
    if (!win_ok(a != INVALID_FILE_ATTRIBUTES, err))
        return false;
    DWORD touched = mask & FS_AM_MASK & ~(DWORD)FILE_ATTRIBUTE_DIRECTORY;
    a = (a & ~touched) | (attr & touched);
    if (!a)
        a = FILE_ATTRIBUTE_NORMAL;
    return win_ok(SetFileAttributesW(w, a), err);
}

/* Set the modification time from the FAT date/time -- the same conversion
 * info_from_find does, run backwards. Windows can set the creation time the
 * API also carries, so it does. */
static bool drive_utime(const char *path, const FILINFO *fno, api_errno *err)
{
    wchar_t w[WIN_WPATH_MAX];
    if (!path_to_wide(path, w, WIN_WPATH_MAX, err))
        return false;
    FILETIME lft, ft;
    if (!win_ok(DosDateTimeToFileTime(fno->fdate, fno->ftime, &lft), err) ||
        !win_ok(LocalFileTimeToFileTime(&lft, &ft), err))
        return false;
    HANDLE h = CreateFileW(w, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (!win_ok(h != INVALID_HANDLE_VALUE, err))
        return false;
    BOOL ok = SetFileTime(h, NULL, NULL, &ft);
    DWORD e = GetLastError();
    CloseHandle(h);
    if (!ok)
    {
        *err = win_error_to_api(e);
        return false;
    }
    return true;
}

static bool drive_getcwd(char *buf, size_t size, api_errno *err)
{
    wchar_t w[WIN_WPATH_MAX];
    DWORD n = GetCurrentDirectoryW(WIN_WPATH_MAX, w);
    if (n == 0 || n >= WIN_WPATH_MAX)
    {
        *err = n ? API_ENOMEM : win_last_error_to_api();
        return false;
    }
    char native[HOST_MAX_PATH], cwd[HOST_MAX_PATH];
    oem_from_wide((const uint16_t *)w, native, sizeof native);
    win_to_slash(native);
    if (!path_from_native(native, cwd, sizeof cwd) ||
        strlen(cwd) >= size) /* did not fit: full-path-or-error */
    {
        *err = API_ENOMEM;
        return false;
    }
    strcpy(buf, cwd);
    return true;
}

/* A Windows volume has a label, but it is not the FAT label the API means and
 * a program cannot act on the difference. Report an empty one and accept
 * (ignore) a set, so label-aware programs run rather than erroring -- these
 * are answers, not missing calls, which is why neither slot is left NULL. */
static bool drive_getlabel(const char *path, char *label, size_t size, api_errno *err)
{
    (void)path, (void)size, (void)err;
    label[0] = 0;
    return true;
}

static bool drive_setlabel(const char *path, api_errno *err)
{
    (void)path, (void)err;
    return true;
}

static bool drive_getfree(const char *path, uint32_t *tot_sect, uint32_t *fre_sect,
                          api_errno *err)
{
    /* A drive query names a drive, and no name is the one in use -- the same
     * rule opendir follows, and what f_getfree does with "". */
    wchar_t w[WIN_WPATH_MAX];
    if (!path_to_wide(path[0] ? path : ".", w, WIN_WPATH_MAX, err))
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
    if (!win_ok(GetDiskFreeSpaceExW(dir, &avail, &total, NULL), err))
        return false;
    uint64_t tot = total.QuadPart / 512;
    uint64_t fre = avail.QuadPart / 512;
    *tot_sect = tot > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)tot;
    *fre_sect = fre > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)fre;
    return true;
}

/* A Windows filesystem takes filenames as UTF-16; there is no page to set. */
void oem_fs_code_page(uint16_t cp)
{
    (void)cp;
}

const dir_backend_t drive_backend = {
    .stat = drive_stat,
    .unlink = drive_unlink,
    .rename = drive_rename,
    .mkdir = drive_mkdir,
    .chdir = drive_chdir,
    .chdrive = drive_chdrive,
    .chmod = drive_chmod,
    .utime = drive_utime,
    .getfree = drive_getfree,
    .getcwd = drive_getcwd,
    .getlabel = drive_getlabel,
    .setlabel = drive_setlabel,
    .opendir = drive_opendir,
    .readdir = drive_readdir,
    .closedir = drive_closedir,
    .rewinddir = drive_rewinddir,
    .validate = drive_validate,
};
