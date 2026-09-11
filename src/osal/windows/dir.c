/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Win32 keeps everything an f_stat_t holds, so a find record is copied rather
 * than reconstructed: FILE_ATTRIBUTE_READONLY, _HIDDEN, _SYSTEM, _DIRECTORY
 * and _ARCHIVE are the FAT attribute bits with the values FAT gave them, and
 * FileTimeToDosDateTime produces the FAT date and time.
 *
 * A path crosses in the 6502's OEM code page and is otherwise spelled the way
 * Win32 wants it, because this host puts the drive letter in the path itself.
 * Only the code page changes, and backslashes become slashes on the way out.
 * A forward slash needs no conversion on the way in, because Win32 normalizes
 * every path it is given and folds slashes to backslashes; that would stop if
 * anything here emitted the \\?\ prefix, and nothing does.
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

wchar_t *path_to_wide(const char *path, api_errno *err)
{
    /* A leading ":" is the null drive, where installed ROMs live, and it has
     * no native spelling. Win32 would read it as an alternate data stream and
     * succeed, so it is refused before Win32 sees it. */
    if (path[0] == ':')
    {
        *err = API_ENODEV;
        return NULL;
    }
    /* A byte with no character in the code page would be substituted, and a
     * substituted name is a different name. */
    if (strlen(path) > API_PATH_MAX || !oem_maps_oem(path))
    {
        *err = API_EINVAL;
        return NULL;
    }
    size_t wcount = strlen(path) + 1; /* one UTF-16 unit per OEM byte */
    wchar_t *w = malloc(wcount * sizeof *w);
    if (w)
        oem_to_wide(path, (uint16_t *)w, (int)wcount);
    else
        *err = API_ENOMEM;
    return w;
}

/* One OEM byte per UTF-16 unit bounds the answer, so this allocation fits. */
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

/* A path in full, resolved the way Win32 resolves one: a relative path against
 * the process working directory, and a drive-relative one ("C:") against the
 * directory Win32 remembers for that drive. The sizing call returns zero on
 * failure and otherwise a count that includes the terminating null. */
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
    if (!got || got >= n) /* the path grew between the two calls */
    {
        *err = got ? API_ENOMEM : win_last_error_to_api();
        free(full);
        return NULL;
    }
    return full;
}

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

static bool win_ok(BOOL ok, api_errno *err)
{
    if (!ok)
        *err = win_last_error_to_api();
    return ok != FALSE;
}

/* The mask keeps the five FAT attribute bits, which Win32 numbers the same,
 * and drops the Windows-only ones, such as COMPRESSED and REPARSE_POINT, from
 * a field a program reads as FAT's. */
#define FS_AM_MASK 0x37 /* RDO|HID|SYS|DIR|ARC */

/* A find reports UTC and FAT records local time, which is what this API
 * carries, so the stamp is converted on the way. FileTimeToDosDateTime fails
 * outside 1980 to 2107, the only years a FAT date can spell, and the stamp is
 * clamped to that range rather than left as the zero this API reads as
 * "no date". */
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
    /* A FILETIME counts 100 ns units from 1601-01-01, so 1980-01-01 is 138426
     * days on, and anything below that is too early. */
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

/* False when the entry's name has no spelling in the running code page,
 * because a substituted name would let two entries arrive under one name and
 * let a program hand back a name that opens neither. */
static bool info_from_find(f_stat_t *info, const WIN32_FIND_DATAW *fd)
{
    if (!oem_maps_wide((const uint16_t *)fd->cFileName))
        return false;
    oem_from_wide((const uint16_t *)fd->cFileName, info->fname, sizeof info->fname);
    /* Win32 leaves cAlternateFileName empty when the long name is already an
     * 8.3 name. */
    oem_from_wide((const uint16_t *)fd->cAlternateFileName, info->altname,
                  sizeof info->altname);
    uint64_t size = ((uint64_t)fd->nFileSizeHigh << 32) | fd->nFileSizeLow;
    /* A directory has size 0 on FAT. */
    info->fsize = (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0
                  : size > 0xFFFFFFFF                               ? 0xFFFFFFFF
                                                                    : (uint32_t)size;
    info->fattrib = (uint8_t)(fd->dwFileAttributes & FS_AM_MASK);
    fat_pack_time(&fd->ftLastWriteTime, &info->fdate, &info->ftime);
    fat_pack_time(&fd->ftCreationTime, &info->crdate, &info->crtime);
    return true;
}

struct win_dir
{
    bool used;
    HANDLE h;
    WIN32_FIND_DATAW fd;
    bool first; /* FindFirstFileW already yielded the first entry */
    bool alive;
    wchar_t *pattern;
    char path[API_PATH_MAX + 1];
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
    /* A find rather than GetFileAttributesEx, because only the find carries
     * the entry's own name in the case the volume stores, with the 8.3 name
     * beside it, which is what readdir reports and what stat has to agree
     * with. FindFirstFileW refuses a trailing separator, so it comes off. */
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
            *err = API_EINVAL;
            return false;
        }
        return true;
    }
    /* A root has no entry of its own to find. Only "/" reliably arrives here:
     * the strip above leaves "C:/" as a bare "C:", which Win32 resolves against
     * the directory it remembers for that drive. */
    WIN32_FILE_ATTRIBUTE_DATA fad;
    bool got = win_ok(GetFileAttributesExW(w, GetFileExInfoStandard, &fad), err);
    free(w);
    if (!got)
        return false;
    memset(&fd, 0, sizeof fd);
    /* A name still has to be reported, and it must not be the empty one
     * readdir uses for end of directory. */
    fd.cFileName[0] = L'/';
    fd.dwFileAttributes = fad.dwFileAttributes;
    fd.ftLastWriteTime = fad.ftLastWriteTime;
    fd.ftCreationTime = fad.ftCreationTime;
    fd.nFileSizeHigh = fad.nFileSizeHigh;
    fd.nFileSizeLow = fad.nFileSizeLow;
    info_from_find(info, &fd); /* "/" always maps, so this cannot fail */
    return true;
}

static bool dir_open_into(int i, const char *path, api_errno *err)
{
    struct win_dir *d = &dirs[i];
    wchar_t *rel = path_to_wide(path[0] ? path : ".", err);
    if (!rel)
        return false;
    /* win_full_path runs before the glob is built, because the pattern the
     * slot keeps has to be absolute: drive_rewinddir would otherwise resolve it
     * again against a working directory the program has since changed. */
    wchar_t *base = win_full_path(rel, err);
    free(rel);
    if (!base)
        return false;
    size_t n = wcslen(base);
    while (n > 1 && (base[n - 1] == L'\\' || base[n - 1] == L'/'))
        n--;
    wchar_t *pattern = malloc((n + 3) * sizeof *pattern); /* backslash, star, null */
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
    d->pattern = pattern;
    d->used = true;
    d->first = true;
    d->alive = true;
    d->path[0] = 0;
    char *abs = os_dir_realpath(path);
    if (abs)
    {
        if (strlen(abs) <= API_PATH_MAX)
            strcpy(d->path, abs);
        free(abs);
    }
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
    if (!dir_open_into(i, path, err))
        return false;
    *des = i;
    return true;
}

bool drive_dir_path(int des, char *buf, size_t size)
{
    if (des < 0 || des >= DIR_MAX_OPEN || !dirs[des].used ||
        !dirs[des].path[0] || strlen(dirs[des].path) >= size)
        return false;
    strcpy(buf, dirs[des].path);
    return true;
}

bool drive_reopendir(int des, const char *path, api_errno *err)
{
    if (des < 0 || des >= DIR_MAX_OPEN)
    {
        *err = API_EINVAL;
        return false;
    }
    if (dirs[des].used)
        drive_closedir(des, err);
    return dir_open_into(des, path, err);
}

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
                    memset(info, 0, sizeof(*info)); /* an empty fname is the end */
                    return true;
                }
                *err = win_error_to_api(e);
                return false;
            }
        }
        d->first = false;
        if (!info_from_find(info, &d->fd))
        {
            *err = API_EINVAL;
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
        /* The API unlinks files and directories with one call. DeleteFileW
         * refuses a directory with ERROR_ACCESS_DENIED, so that is the only
         * error worth retrying as RemoveDirectoryW, and the retry's own error
         * replaces it because ERROR_DIR_NOT_EMPTY is more use than a plain
         * access refusal. */
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
    bool ok = win_ok(SetCurrentDirectoryW(w), err);
    free(w);
    return ok;
}

/* The drives are Windows' own, so this is Windows' own change-drive:
 * SetCurrentDirectoryW of a bare "X:", which is what cd /d and the CRT's
 * _chdrive do. Win32 resolves a bare drive against the directory it remembers
 * for that drive in the hidden "=X:" environment variable, and lands on the
 * drive's root when it remembers none, which is the usual case for a process
 * not started from cmd.exe.
 *
 * The letter is checked against the mounted set first, so a drive that is not
 * there reports a missing device rather than a path error. */
bool drive_chdrive(const char *drive, api_errno *err)
{
    if (!drive[0])
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

/* The API's attribute bits are Win32's own bits, so the ones the mask does not
 * name are left as Windows has them. */
bool drive_chmod(const char *path, uint8_t attr, uint8_t mask, api_errno *err)
{
    wchar_t *w = path_to_wide(path, err);
    if (!w)
        return false;
    /* The path is resolved even when the mask names nothing this can change,
     * because a chmod of something that is not there is still an error. */
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

static bool fat_to_filetime(uint16_t date, uint16_t time, FILETIME *ft, api_errno *err)
{
    FILETIME lft;
    return win_ok(DosDateTimeToFileTime(date, time, &lft), err) &&
           win_ok(LocalFileTimeToFileTime(&lft, ft), err);
}

/* A date of 0 is not a date, and the API leaves that stamp unchanged, which is
 * also what f_utime does. SetFileTime asks for the same thing with NULL. */
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
    if (!got || got >= n) /* the directory changed between the two calls */
    {
        *err = got ? API_ENOMEM : win_last_error_to_api();
        free(w);
        return false;
    }
    bool ok = oem_from_wide((const uint16_t *)w, buf, size) < size;
    if (ok)
        win_to_slash(buf);
    else
        *err = API_ENOMEM;
    free(w);
    return ok;
}

static wchar_t *win_volume(const char *path, api_errno *err)
{
    wchar_t *rel = path_to_wide(path[0] ? path : ".", err);
    if (!rel)
        return NULL;
    wchar_t *full = win_full_path(rel, err);
    free(rel);
    if (!full)
        return NULL;
    /* GetVolumePathNameW answers with a prefix of the path it is given, so the
     * expanded path's own length is always room enough. */
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

/* On a FAT or exFAT volume this is the label FAT itself stores, so the same
 * stick reads the same label on a Picocomputer. */
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

/* FatFs takes this argument as "[drive:]label", so the name is what follows
 * the colon. */
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
    /* SetVolumeLabelW clears the label when it is given NULL. */
    bool ok = win_ok(SetVolumeLabelW(root, w[0] ? w : NULL), err);
    free(root), free(w);
    return ok;
}

bool drive_getfree(const char *path, uint32_t *tot_sect, uint32_t *fre_sect,
                          api_errno *err)
{
    /* win_full_path settles a bare "C:" as that drive's own directory rather
     * than its root, the way every other call here reads one. */
    wchar_t *rel = path_to_wide(path[0] ? path : ".", err);
    if (!rel)
        return false;
    wchar_t *w = win_full_path(rel, err);
    free(rel);
    if (!w)
        return false;
    /* GetDiskFreeSpaceExW wants a directory, so the last component comes off
     * whatever it names. The parent is on the same volume, which is all this
     * asks about. */
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

/* A Windows filesystem takes filenames as UTF-16, so there is no code page to
 * set. */
void oem_fs_code_page(uint16_t cp)
{
    (void)cp;
}

