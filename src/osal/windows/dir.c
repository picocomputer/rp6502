/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This machine's drive, as osal/dir.h asks for it: the directory syscalls
 * answered over Win32. The counterpart of osal/posix/dir.c.
 *
 * Windows keeps what the 6502 asks for. FILE_ATTRIBUTE_READONLY, _HIDDEN,
 * _SYSTEM, _DIRECTORY and _ARCHIVE are the FAT attribute bits, with the same
 * values FAT gave them, and FileTimeToDosDateTime is the FAT date and time --
 * so an f_stat_t is read off a find record rather than reconstructed, and
 * none of it is a guess. A find carries all of it, so a read costs no extra
 * call.
 *
 * There is no opendir/readdir on Win32: FindFirstFileW/FindNextFileW/FindClose
 * over an opaque heap struct.
 *
 * Paths cross spelled the way the 6502 spells them and in its OEM code page.
 * The drive prefix comes off with path_to_native() and the code page with
 * oem_to_wide() (core/str/oem.h) before every ...W call; returned names go
 * back with oem_from_wide().
 */

#include "osal/dir.h"
#include "core/str/oem.h"
#include "core/str/path.h"
#include "osal/os.h"
#include "osal/windows/dir.h"
#include "osal/windows/errmap.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <strings.h>
#include <windows.h>

#define DIR_NAME_MAX 256 /* an entry's name, not a path */

/* A path arrives spelled the way the 6502 spells it. This drive is one
 * directory of a real filesystem, so the drive prefix comes off here and what
 * is left is the native path -- and then the code page comes off too. */
wchar_t *path_to_wide(const char *path, api_errno *err)
{
    size_t nsz = strlen(path) + 1;
    char *native = malloc(nsz);
    if (!native)
    {
        *err = API_ENOMEM;
        return NULL;
    }
    if (!path_to_native(path, native, nsz))
    {
        free(native);
        *err = API_EINVAL;
        return NULL;
    }
    size_t wcount = strlen(native) + 1; /* one unit per OEM byte */
    wchar_t *w = malloc(wcount * sizeof *w);
    if (w)
    {
        oem_to_wide(native, (uint16_t *)w, (int)wcount);
        if (!w[0]) /* a path of no name reaches no file here */
        {
            free(w);
            w = NULL;
            *err = API_ENOENT;
        }
    }
    else
        *err = API_ENOMEM;
    free(native);
    return w;
}

/* And back: what Win32 answered, slashed and spelled for the 6502. One length
 * answers for both steps -- one OEM byte per unit, and path_from_native
 * prepends at most a six-byte drive prefix. */
char *path_from_wide(const wchar_t *w, api_errno *err)
{
    size_t sz = wcslen(w) + 7;
    char *native = malloc(sz), *out = malloc(sz);
    if (native && out)
    {
        oem_from_wide((const uint16_t *)w, native, sz);
        win_to_slash(native);
        if (!path_from_native(native, out, sz))
        {
            free(out);
            out = NULL;
            *err = API_EINVAL; /* a name too long, as win_error_to_api spells it */
        }
    }
    else
    {
        free(out);
        out = NULL;
        *err = API_ENOMEM;
    }
    free(native);
    return out;
}

void win_to_slash(char *p)
{
    for (; *p; p++)
        if (*p == '\\')
            *p = '/';
}

/* Absolute, in the 6502's spelling -- what argv[0] needs to survive a chdir.
 * Resolved against the cwd drive_getcwd answers for, which is why it is here.
 * No error channel: a caller has a path or it has nothing. */
char *os_dir_realpath(const char *path)
{
    api_errno ignored;
    wchar_t *wpath = path_to_wide(path, &ignored);
    if (!wpath)
        return NULL;
    /* Asked for its own length first: zero means failure, otherwise it counts
     * the terminating null, which is exactly what the second call wants. */
    DWORD n = GetFullPathNameW(wpath, 0, NULL, NULL);
    wchar_t *wfull = n ? malloc((size_t)n * sizeof *wfull) : NULL;
    DWORD got = wfull ? GetFullPathNameW(wpath, n, wfull, NULL) : 0;
    /* got >= n means it grew since the sizing call and asked again */
    char *out = (got && got < n) ? path_from_wide(wfull, &ignored) : NULL;
    free(wpath), free(wfull);
    return out;
}

char *os_dir_path_hold(const char *path)
{
    return strdup(path);
}

void os_dir_path_drop(char *path)
{
    free(path);
}

/* Whatever Win32 last complained about, in the API's words. */
static bool win_ok(BOOL ok, api_errno *err)
{
    if (!ok)
        *err = win_last_error_to_api();
    return ok != FALSE;
}

/* ---- f_stat_t, straight off what Win32 keeps ----------------------------- */

/* The FAT attribute bits the 6502 sees, which are the same bits Win32 uses --
 * masked so nothing Windows-only (COMPRESSED, REPARSE_POINT, ...) leaks into
 * a field a program reads as FAT's. */
#define FS_AM_MASK 0x37 /* RDO|HID|SYS|DIR|ARC */

static void info_from_find(f_stat_t *info, const WIN32_FIND_DATAW *fd, const char *name)
{
    snprintf(info->fname, sizeof(info->fname), "%s", name);
    info->altname[0] = 0; /* the 8.3 name Win32 offers is not asked for here */
    uint64_t size = ((uint64_t)fd->nFileSizeHigh << 32) | fd->nFileSizeLow;
    info->fsize = size > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)size;
    info->fattrib = (uint8_t)(fd->dwFileAttributes & FS_AM_MASK);
    /* A find reports UTC; FAT records local time, which is what the API
     * carries, so each stamp goes through the local conversion on the way. */
    FILETIME lft;
    WORD d = 0, t = 0;
    if (FileTimeToLocalFileTime(&fd->ftLastWriteTime, &lft))
        FileTimeToDosDateTime(&lft, &d, &t);
    info->fdate = d;
    info->ftime = t;
    d = t = 0;
    if (FileTimeToLocalFileTime(&fd->ftCreationTime, &lft))
        FileTimeToDosDateTime(&lft, &d, &t);
    info->crdate = d;
    info->crtime = t;
}

/* ---- The drive, as core/api/dir.c asks for it ---------------------------- */

struct win_dir
{
    bool used;
    HANDLE h;
    WIN32_FIND_DATAW fd;
    bool first; /* FindFirstFileW already yielded the first entry */
    bool alive;
    wchar_t *pattern; /* owned, the FindFirstFile glob this slot rewinds to */
};
static struct win_dir dirs[DIR_MAX_OPEN];

bool drive_validate(int des, api_errno *err)
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

bool drive_stat(const char *path, f_stat_t *info, api_errno *err)
{
    wchar_t *w = path_to_wide(path, err);
    if (!w)
        return false;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    bool got = win_ok(GetFileAttributesExW(w, GetFileExInfoStandard, &fad), err);
    free(w);
    if (!got)
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
    info_from_find(info, &fd, path_basename(path));
    return true;
}

bool drive_opendir(const char *path, int *des, api_errno *err)
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
    /* a directory of no name is the working directory */
    wchar_t *base = path_to_wide(path_strip_drive(path)[0] ? path : ".", err);
    if (!base)
        return false;
    size_t n = wcslen(base);
    while (n > 0 && (base[n - 1] == L'\\' || base[n - 1] == L'/'))
        n--;
    wchar_t *pattern = malloc((n + 3) * sizeof *pattern); /* + \\ * and the null */
    if (!pattern)
    {
        free(base);
        *err = API_ENOMEM;
        return false;
    }
    memcpy(pattern, base, n * sizeof(wchar_t));
    pattern[n++] = L'\\';
    pattern[n++] = L'*';
    pattern[n] = 0;
    free(base);

    d->h = FindFirstFileW(pattern, &d->fd);
    if (!win_ok(d->h != INVALID_HANDLE_VALUE, err))
    {
        free(pattern);
        return false;
    }
    d->pattern = pattern; /* the slot was free, so closedir already released one */
    d->used = true;
    d->first = true;
    d->alive = true;
    *des = i;
    return true;
}

/* "." and ".." are not entries the 6502 sees. */
bool drive_readdir(int des, f_stat_t *info, api_errno *err)
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
                    memset(info, 0, sizeof(*info)); /* fname[0]==0 signals EOF */
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
        info_from_find(info, &d->fd, name);
        return true;
    }
}

bool drive_closedir(int des, api_errno *err)
{
    (void)err;
    struct win_dir *d = &dirs[des];
    if (d->alive && d->h != INVALID_HANDLE_VALUE)
        FindClose(d->h);
    free(d->pattern);
    d->pattern = NULL;
    d->used = false;
    d->alive = false;
    d->h = INVALID_HANDLE_VALUE;
    return true;
}

bool drive_rewinddir(int des, api_errno *err)
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

bool drive_unlink(const char *path, api_errno *err)
{
    wchar_t *w = path_to_wide(path, err);
    if (!w)
        return false;
    bool ok = DeleteFileW(w);
    if (!ok)
    {
        DWORD e = GetLastError();
        /* One call for both, the way the API asks: a directory refuses
         * DeleteFile with an access complaint, and RemoveDirectory is what it
         * wanted. */
        ok = e == ERROR_ACCESS_DENIED && RemoveDirectoryW(w);
        if (!ok)
            *err = win_error_to_api(e);
    }
    free(w);
    return ok;
}

bool drive_rename(const char *oldname, const char *newname, api_errno *err)
{
    wchar_t *wo = path_to_wide(oldname, err);
    if (!wo)
        return false;
    wchar_t *wn = path_to_wide(newname, err);
    if (!wn)
    {
        free(wo);
        return false;
    }
    bool ok = win_ok(MoveFileExW(wo, wn, MOVEFILE_REPLACE_EXISTING), err);
    free(wo), free(wn);
    return ok;
}

bool drive_mkdir(const char *path, api_errno *err)
{
    wchar_t *w = path_to_wide(path, err);
    if (!w)
        return false;
    bool ok = win_ok(CreateDirectoryW(w, NULL), err);
    free(w);
    return ok;
}

bool drive_chdir(const char *path, api_errno *err)
{
    wchar_t *w = path_to_wide(path, err);
    if (!w)
        return false;
    /* validates existence and dir-ness */
    bool ok = win_ok(SetCurrentDirectoryW(w), err);
    free(w);
    return ok;
}

/* The 6502 sees MSC0: (and the bare current drive); anything else is a
 * missing device. */
bool drive_chdrive(const char *drive, api_errno *err)
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
bool drive_chmod(const char *path, uint8_t attr, uint8_t mask, api_errno *err)
{
    if (!(mask & FS_AM_MASK))
        return true;
    wchar_t *w = path_to_wide(path, err);
    if (!w)
        return false;
    DWORD a = GetFileAttributesW(w);
    bool ok = win_ok(a != INVALID_FILE_ATTRIBUTES, err);
    if (ok)
    {
        DWORD touched = mask & FS_AM_MASK & ~(DWORD)FILE_ATTRIBUTE_DIRECTORY;
        a = (a & ~touched) | (attr & touched);
        if (!a)
            a = FILE_ATTRIBUTE_NORMAL;
        ok = win_ok(SetFileAttributesW(w, a), err);
    }
    free(w);
    return ok;
}

/* Set the modification time from the FAT date/time -- the same conversion
 * info_from_find does, run backwards. Windows can set the creation time the
 * API also carries, so it does. */
bool drive_utime(const char *path, const f_stat_t *info, api_errno *err)
{
    wchar_t *w = path_to_wide(path, err);
    if (!w)
        return false;
    FILETIME lft, ft;
    if (!win_ok(DosDateTimeToFileTime(info->fdate, info->ftime, &lft), err) ||
        !win_ok(LocalFileTimeToFileTime(&lft, &ft), err))
    {
        free(w);
        return false;
    }
    HANDLE h = CreateFileW(w, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    bool opened = win_ok(h != INVALID_HANDLE_VALUE, err);
    free(w);
    if (!opened)
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

bool drive_getcwd(char *buf, size_t size, api_errno *err)
{
    /* Asked for its own length first: zero means failure, otherwise it counts
     * the terminating null, which is what the second call wants. */
    DWORD n = GetCurrentDirectoryW(0, NULL);
    if (!n)
    {
        *err = win_last_error_to_api();
        return false;
    }
    wchar_t *w = malloc((size_t)n * sizeof *w);
    if (!w)
    {
        *err = API_ENOMEM;
        return false;
    }
    DWORD got = GetCurrentDirectoryW(n, w);
    if (!got || got >= n) /* grew since the sizing call: it asked again */
    {
        *err = got ? API_ENOMEM : win_last_error_to_api();
        free(w);
        return false;
    }
    /* one OEM byte per unit, and path_from_native prepends at most six */
    size_t sz = wcslen(w) + 7;
    char *native = malloc(sz), *cwd = malloc(sz);
    bool ok = native && cwd;
    if (ok)
    {
        oem_from_wide((const uint16_t *)w, native, sz);
        win_to_slash(native);
        ok = path_from_native(native, cwd, sz) &&
             strlen(cwd) < size; /* did not fit: full-path-or-error */
    }
    if (ok)
        strcpy(buf, cwd);
    else
        *err = API_ENOMEM;
    free(w), free(native), free(cwd);
    return ok;
}

/* A Windows volume has a label, but it is not the FAT label the API means and
 * a program cannot act on the difference. Report an empty one and accept
 * (ignore) a set, so label-aware programs run rather than erroring -- these
 * are answers, not missing calls, which is why neither slot is left NULL. */
bool drive_getlabel(const char *path, char *label, size_t size, api_errno *err)
{
    (void)path, (void)size, (void)err;
    label[0] = 0;
    return true;
}

bool drive_setlabel(const char *path, api_errno *err)
{
    (void)path, (void)err;
    return true;
}

bool drive_getfree(const char *path, uint32_t *tot_sect, uint32_t *fre_sect,
                          api_errno *err)
{
    /* A drive query names a drive, and no name is the one in use -- the same
     * rule opendir follows, and what f_getfree does with "". */
    wchar_t *w = path_to_wide(path[0] ? path : ".", err);
    if (!w)
        return false;
    /* Prefer the parent directory when path names a file. Truncated in place:
     * the copy this used to make of itself was the fixed buffer's, not the
     * algorithm's. */
    wchar_t *slash = wcsrchr(w, L'\\');
    wchar_t *slash2 = wcsrchr(w, L'/');
    if (slash2 && (!slash || slash2 > slash))
        slash = slash2;
    if (slash && slash != w && !(slash == w + 2 && w[1] == L':'))
        *slash = 0;
    ULARGE_INTEGER avail, total;
    bool ok = win_ok(GetDiskFreeSpaceExW(w, &avail, &total, NULL), err);
    free(w);
    if (!ok)
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

