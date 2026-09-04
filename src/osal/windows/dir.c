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
#include "osal/os.h"
#include "osal/windows/dir.h"
#include "osal/windows/errmap.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <wchar.h>
#include <windows.h>

/* A path arrives in the 6502's code page, otherwise as Win32 wants it. */
wchar_t *path_to_wide(const char *path, api_errno *err)
{
    /* A leading ":" is the null drive, where installed ROMs live. It has no
     * native spelling at all, and here it would be an alternate data stream
     * rather than a miss, so it is refused before Win32 sees it. */
    if (path[0] == ':')
    {
        *err = API_ENODEV; /* FR_INVALID_DRIVE, as the Pico spells it */
        return NULL;
    }
    /* A byte the code page cannot spell would be substituted, and a
     * substituted name is a different name. FatFs refuses it; so does this.
     * The length goes with it: API_PATH_MAX is what the machine this API was
     * written for holds, and taking more here was the only way the hosts
     * differed. */
    if (strlen(path) > API_PATH_MAX || !oem_maps_oem(path))
    {
        *err = API_EINVAL; /* FR_INVALID_NAME, as the Pico spells it */
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

/* A find reports UTC; FAT records local time, which is what the API carries,
 * so a stamp goes through the local conversion on the way. The FAT field holds
 * 1980 to 2107 and nothing else -- FileTimeToDosDateTime says so by failing,
 * and a stamp outside that range clamps rather than being handed on as the
 * zero this API reads as "no date". */
static void fat_pack_time(const FILETIME *ft, uint16_t *fdate, uint16_t *ftime)
{
    FILETIME lft;
    WORD d = 0, t = 0;
    if (FileTimeToLocalFileTime(ft, &lft) && FileTimeToDosDateTime(&lft, &d, &t))
    {
        *fdate = d;
        *ftime = t;
        return;
    }
    /* Which end it fell off: a FILETIME counts from 1601, so anything below
     * the 1980 epoch is early and everything else is late. */
    static const uint64_t fat_epoch = 119600064000000000ull; /* 1980-01-01 UTC */
    uint64_t v = ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    if (v < fat_epoch)
    {
        *fdate = (1 << 5) | 1; /* 1980-01-01 */
        *ftime = 0;
    }
    else
    {
        *fdate = (127 << 9) | (12 << 5) | 31; /* 2107-12-31 */
        *ftime = (23 << 11) | (59 << 5) | 29;
    }
}

/* False when the entry's name has no spelling in the running code page: a
 * substituted one would let two entries arrive under a single name and let a
 * program hand back a name that opens neither. */
static bool info_from_find(f_stat_t *info, const WIN32_FIND_DATAW *fd)
{
    if (!oem_maps_wide((const uint16_t *)fd->cFileName))
        return false;
    /* The name off the record, which is the case the volume really stores. */
    oem_from_wide((const uint16_t *)fd->cFileName, info->fname, sizeof info->fname);
    /* Win32 keeps the 8.3 name FatFs keeps, under the same rule -- empty when
     * the long name is already one -- so it transfers as it stands. */
    oem_from_wide((const uint16_t *)fd->cAlternateFileName, info->altname,
                  sizeof info->altname);
    uint64_t size = ((uint64_t)fd->nFileSizeHigh << 32) | fd->nFileSizeLow;
    /* A directory's size is 0 on FAT, and Win32 reports it that way too. */
    info->fsize = (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0
                  : size > 0xFFFFFFFF                               ? 0xFFFFFFFF
                                                                    : (uint32_t)size;
    info->fattrib = (uint8_t)(fd->dwFileAttributes & FS_AM_MASK);
    fat_pack_time(&fd->ftLastWriteTime, &info->fdate, &info->ftime);
    fat_pack_time(&fd->ftCreationTime, &info->crdate, &info->crtime);
    return true;
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
    /* A find rather than GetFileAttributesEx, because a find is the only one
     * of the two that carries the entry's own name -- in the case the volume
     * stores and with the 8.3 name beside it, which is what readdir reports
     * and therefore what stat has to agree with. It refuses a trailing
     * separator, so that comes off first. */
    size_t n = wcslen(w);
    while (n > 1 && (w[n - 1] == L'\\' || w[n - 1] == L'/'))
        w[--n] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w, &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        FindClose(h);
        free(w);
        if (!info_from_find(info, &fd))
        {
            *err = API_EINVAL; /* FR_INVALID_NAME, as the Pico spells it */
            return false;
        }
        return true;
    }
    /* A root has no entry to find, and no name of its own either. */
    WIN32_FILE_ATTRIBUTE_DATA fad;
    bool got = win_ok(GetFileAttributesExW(w, GetFileExInfoStandard, &fad), err);
    free(w);
    if (!got)
        return false;
    memset(&fd, 0, sizeof fd);
    fd.dwFileAttributes = fad.dwFileAttributes;
    fd.ftLastWriteTime = fad.ftLastWriteTime;
    fd.ftCreationTime = fad.ftCreationTime;
    fd.nFileSizeHigh = fad.nFileSizeHigh;
    fd.nFileSizeLow = fad.nFileSizeLow;
    info_from_find(info, &fd); /* a root, so there is no name to refuse */
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
        if (!info_from_find(info, &d->fd))
        {
            *err = API_EINVAL; /* FR_INVALID_NAME, as the Pico spells it */
            return false;
        }
        if (strcmp(info->fname, ".") == 0 || strcmp(info->fname, "..") == 0)
            continue;
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
         * wanted. Whichever attempt failed last is the one that gets to say
         * why -- a non-empty directory is RemoveDirectory's complaint, and
         * DeleteFile's would have hidden it behind a plain access refusal. */
        if (e == ERROR_ACCESS_DENIED)
        {
            ok = RemoveDirectoryW(w);
            if (!ok)
                e = GetLastError();
        }
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
    wchar_t *w = path_to_wide(path, err);
    if (!w)
        return false;
    /* Resolved even when the mask names nothing this can change: a chmod of
     * something that is not there is an error, and the mask does not get to
     * decide whether a missing file exists. */
    DWORD a = GetFileAttributesW(w);
    bool ok = win_ok(a != INVALID_FILE_ATTRIBUTES, err);
    if (ok && (mask & FS_AM_MASK))
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

/* One FAT date and time to a FILETIME -- the same conversion info_from_find
 * does, run backwards. A date of zero is the API's "leave this stamp alone",
 * so it is not a failure and produces no time to set. */
static bool fat_to_filetime(uint16_t date, uint16_t time, FILETIME *ft, api_errno *err)
{
    FILETIME lft;
    return win_ok(DosDateTimeToFileTime(date, time, &lft), err) &&
           win_ok(LocalFileTimeToFileTime(&lft, ft), err);
}

/* Set the stamps the API carries, both of which Windows keeps. A date of 0 is
 * invalid and leaves that stamp unchanged, which is what the API promises and
 * what f_utime does; SetFileTime says the same thing with a null pointer. */
bool drive_utime(const char *path, const f_stat_t *info, api_errno *err)
{
    wchar_t *w = path_to_wide(path, err);
    if (!w)
        return false;
    FILETIME mft, cft;
    bool ok = (!info->fdate || fat_to_filetime(info->fdate, info->ftime, &mft, err)) &&
              (!info->crdate || fat_to_filetime(info->crdate, info->crtime, &cft, err));
    if (!ok)
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
    BOOL set = SetFileTime(h, info->crdate ? &cft : NULL, NULL,
                           info->fdate ? &mft : NULL);
    DWORD e = GetLastError();
    CloseHandle(h);
    if (!set)
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

/* The volume a path is on, as a root Win32 will take. */
static wchar_t *win_volume(const char *path, api_errno *err)
{
    wchar_t *rel = path_to_wide(path[0] ? path : ".", err);
    if (!rel)
        return NULL;
    wchar_t *full = win_full_path(rel, err);
    free(rel);
    if (!full)
        return NULL;
    /* GetVolumePathName writes into the caller's buffer and can only shorten
     * what it was given, so the expanded path is its own room. */
    size_t n = wcslen(full) + 1;
    wchar_t *root = malloc(n * sizeof *root);
    if (!root)
    {
        *err = API_ENOMEM;
        free(full);
        return NULL;
    }
    bool ok = win_ok(GetVolumePathNameW(full, root, (DWORD)n), err);
    free(full);
    if (!ok)
    {
        free(root);
        return NULL;
    }
    return root;
}

/* A volume here really does have a label, and on a FAT or exFAT one it is the
 * very label the API means -- the same stick reads PICO on a Picocomputer. It
 * is reported as it is found, truncated to the eleven characters the API's
 * field holds, which is what FAT holds too. */
bool drive_getlabel(const char *path, char *label, size_t size, api_errno *err)
{
    wchar_t *root = win_volume(path, err);
    if (!root)
        return false;
    wchar_t name[MAX_PATH + 1];
    bool ok = win_ok(GetVolumeInformationW(root, name, MAX_PATH + 1, NULL, NULL,
                                           NULL, NULL, 0),
                     err);
    free(root);
    if (ok)
        oem_from_wide((const uint16_t *)name, label, size);
    return ok;
}

/* The name is what follows the drive, the way FatFs takes "[drive:]label". */
bool drive_setlabel(const char *path, api_errno *err)
{
    const char *name = strchr(path, ':');
    name = name ? name + 1 : path;
    wchar_t *root = win_volume(path, err);
    if (!root)
        return false;
    size_t n = strlen(name) + 1;
    wchar_t *w = malloc(n * sizeof *w);
    if (!w)
    {
        *err = API_ENOMEM;
        free(root);
        return false;
    }
    oem_to_wide(name, (uint16_t *)w, (int)n);
    /* An empty name clears the label, which is what a null asks for. */
    bool ok = win_ok(SetVolumeLabelW(root, w[0] ? w : NULL), err);
    free(root), free(w);
    return ok;
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

