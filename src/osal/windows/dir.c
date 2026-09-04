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
 * Paths cross in the 6502's OEM code page, and are already spelled the way
 * Win32 wants them: this host puts a drive letter in a path itself, so there
 * is nothing to take off or put back. Only the code page changes, with
 * oem_to_wide() / oem_from_wide() (core/str/oem.h), and backslashes become
 * slashes on the way out. Forward slashes need no conversion on the way in --
 * every Win32 path is normalized through RtlGetFullPathName, which folds them
 * -- and nothing here emits the \\?\ prefix that would turn that off.
 */

#include "osal/dir.h"
#include "core/str/oem.h"
#include "core/str/path.h"
#include "osal/os.h"
#include "osal/windows/dir.h"
#include "osal/windows/errmap.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#define DIR_NAME_MAX 256 /* an entry's name, not a path */

/* A path arrives in the 6502's code page, otherwise as Win32 wants it. */
wchar_t *path_to_wide(const char *path, api_errno *err)
{
    /* A leading ":" is the null drive, where installed ROMs live. It has no
     * native spelling at all, and here it would be an alternate data stream
     * rather than a miss, so it is refused before Win32 sees it. */
    if (path[0] == ':')
    {
        *err = API_EINVAL;
        return NULL;
    }
    size_t wcount = strlen(path) + 1; /* one unit per OEM byte */
    wchar_t *w = malloc(wcount * sizeof *w);
    if (w)
        oem_to_wide(path, (uint16_t *)w, (int)wcount);
    else
        *err = API_ENOMEM;
    return w;
}

/* And back: what Win32 answered, slashed and in the 6502's code page. One OEM
 * byte per unit bounds the answer, and nothing is prepended. */
char *path_from_wide(const wchar_t *w, api_errno *err)
{
    size_t sz = wcslen(w) + 1;
    char *out = malloc(sz);
    if (out)
    {
        oem_from_wide((const uint16_t *)w, out, sz);
        win_to_slash(out);
    }
    else
        *err = API_ENOMEM;
    return out;
}

void win_to_slash(char *p)
{
    for (; *p; p++)
        if (*p == '\\')
            *p = '/';
}

/* What Win32 makes of a path when it is asked to say it in full: relative
 * against the process cwd, drive-relative ("C:") against that drive's own
 * remembered directory. Sized by asking first -- zero means failure, and
 * otherwise the count includes the terminating null. */
static wchar_t *win_full_path(const wchar_t *w, api_errno *err)
{
    DWORD n = GetFullPathNameW(w, 0, NULL, NULL);
    if (!n)
    {
        *err = win_last_error_to_api();
        return NULL;
    }
    wchar_t *full = malloc((size_t)n * sizeof *full);
    if (!full)
    {
        *err = API_ENOMEM;
        return NULL;
    }
    DWORD got = GetFullPathNameW(w, n, full, NULL);
    if (!got || got >= n) /* grew since the sizing call: it asked again */
    {
        *err = got ? API_ENOMEM : win_last_error_to_api();
        free(full);
        return NULL;
    }
    return full;
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
    wchar_t *wfull = win_full_path(wpath, &ignored);
    free(wpath);
    if (!wfull)
        return NULL;
    char *out = path_from_wide(wfull, &ignored);
    free(wfull);
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
    wchar_t *rel = path_to_wide(path[0] ? path : ".", err);
    if (!rel)
        return false;
    /* Expanded before the glob is built, which answers two questions at once:
     * a bare "C:" is that drive's own directory rather than its root, the way
     * every other call here reads it; and the pattern the slot keeps is
     * absolute, so drive_rewinddir cannot re-resolve it against wherever the
     * program has since gone. */
    wchar_t *base = win_full_path(rel, err);
    free(rel);
    if (!base)
        return false;
    size_t n = wcslen(base);
    while (n > 1 && (base[n - 1] == L'\\' || base[n - 1] == L'/'))
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

/* The drives here are Windows' own, so this is Windows' own change-drive:
 * SetCurrentDirectory of a bare "X:", which is what cd /d and the CRT's
 * _chdrive are. Win32 reads a bare drive against the directory it remembers
 * for that drive in the hidden "=X:" variable, and lands on the drive's root
 * when it remembers none -- which is the usual case for a process that was
 * not started from cmd.exe. That is the platform's answer and this takes it.
 *
 * The letter is checked against the mounted set first, so a drive that is not
 * there is a missing device rather than whatever a path error would say. One
 * that is there but not ready then reports what Win32 thinks of it. */
bool drive_chdrive(const char *drive, api_errno *err)
{
    if (!drive[0]) /* no name is the drive in use */
        return true;
    char letter = drive[0];
    bool named = isalpha((unsigned char)letter) &&
                 (!drive[1] || (drive[1] == ':' && !drive[2]));
    if (named && (GetLogicalDrives() & (1u << (toupper((unsigned char)letter) - 'A'))))
    {
        const wchar_t w[3] = {(wchar_t)letter, L':', 0};
        return win_ok(SetCurrentDirectoryW(w), err);
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
    /* What Win32 said, which already carries this host's drive letter. One OEM
     * byte per unit bounds it. */
    bool ok = oem_from_wide((const uint16_t *)w, buf, size) < size;
    if (ok)
        win_to_slash(buf);
    else
        *err = API_ENOMEM; /* did not fit: full-path-or-error */
    free(w);
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
     * rule opendir follows, and what f_getfree does with "". Expanded first,
     * so a bare "C:" is that drive's own directory and GetDiskFreeSpaceEx is
     * given a directory, which is what it asks for. */
    wchar_t *rel = path_to_wide(path[0] ? path : ".", err);
    if (!rel)
        return false;
    wchar_t *w = win_full_path(rel, err);
    free(rel);
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

