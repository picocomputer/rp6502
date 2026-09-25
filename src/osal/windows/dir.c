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
 * A path crosses in the 6502's OEM code page and is otherwise in Win32's path
 * format, because this host puts the drive letter in the path itself.
 * Only the code page changes, and backslashes become slashes on the way out.
 * A forward slash needs no conversion on the way in, because Win32 normalizes
 * every path it is given and folds slashes to backslashes; that would stop if
 * anything here emitted the \\?\ prefix, and nothing does.
 */

#include "osal/dir.h"
#include "core/str/oem.h"
#include "core/str/path.h"
#include "osal/os.h"
#include "osal/windows/dir.h"
#include "osal/windows/errmap.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <wchar.h>
#include <wctype.h>
#include <windows.h>

static bool win_has_drive(const char *path)
{
    unsigned char c = (unsigned char)path[0] | 0x20;
    return c >= 'a' && c <= 'z' && path[1] == ':';
}

static bool win_drive_mounted(char letter)
{
    return (GetLogicalDrives() >> (((unsigned char)letter | 0x20) - 'a')) & 1;
}

/* Win32 opens a device for these final names, with any extension and any
 * trailing spaces, so no file can be stored under one. A trailing separator
 * does not hide one, because Win32 reads "CON\" as a folder named CON, which
 * mkdir would create and no plain path could open again. */
static bool win_reserved(const wchar_t *w)
{
    size_t end = wcslen(w);
    while (end && (w[end - 1] == L'\\' || w[end - 1] == L'/'))
        end--;
    size_t start = end;
    while (start && w[start - 1] != L'\\' && w[start - 1] != L'/' && w[start - 1] != L':')
        start--;
    const wchar_t *name = w + start;
    size_t n = 0;
    while (start + n < end && name[n] != L'.')
        n++;
    while (n && name[n - 1] == L' ')
        n--;
    static const wchar_t *const devices[] = {L"CON", L"PRN", L"AUX", L"NUL",
                                             L"CONIN$", L"CONOUT$"};
    for (size_t i = 0; i < sizeof devices / sizeof devices[0]; i++)
        if (wcslen(devices[i]) == n && !_wcsnicmp(name, devices[i], n))
            return true;
    /* Win32 takes the superscript digits one to three as port numbers too. */
    wchar_t d = n == 4 ? name[3] : 0;
    return ((d >= L'1' && d <= L'9') || d == 0xB9 || d == 0xB2 || d == 0xB3) &&
           (!_wcsnicmp(name, L"COM", 3) || !_wcsnicmp(name, L"LPT", 3));
}

/* The FAT rules are applied before a path is passed to Win32, because Win32
 * gives ':' and the wildcards other meanings: a device, a stream, a pattern.
 * Two leading separators start a UNC path or a \\?\ or \\.\ device path,
 * none of which uses a drive letter. */
wchar_t *path_to_wide(const char *path, api_errno *err)
{
    bool drive = win_has_drive(path);
    if ((drive && !win_drive_mounted(path[0])) ||
        (path_is_sep(path[0]) && path_is_sep(path[1])))
    {
        *err = API_ENODEV;
        return NULL;
    }
    if (!path_fat_ok(drive ? path + 2 : path, drive, err))
        return NULL;
    /* A byte with no character in the code page would be substituted, and a
     * substituted name is a different name. */
    if (strlen(path) > API_PATH_MAX || !oem_maps_oem(path))
    {
        *err = API_EINVAL;
        return NULL;
    }
    size_t wcount = strlen(path) + 1; /* one UTF-16 unit per OEM byte */
    wchar_t *w = malloc(wcount * sizeof *w);
    if (!w)
    {
        *err = API_ENOMEM;
        return NULL;
    }
    oem_to_wide(path, (uint16_t *)w, (int)wcount);
    if (win_reserved(w))
    {
        free(w);
        *err = API_EINVAL;
        return NULL;
    }
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

wchar_t *win_utf8_to_wide(const char *u8, api_errno *err)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u8, -1, NULL, 0);
    if (n <= 0)
    {
        *err = API_EINVAL;
        return NULL;
    }
    wchar_t *w = malloc((size_t)n * sizeof *w);
    if (!w)
    {
        *err = API_ENOMEM;
        return NULL;
    }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u8, -1, w, n);
    return w;
}

char *win_wide_to_utf8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *u8 = n > 0 ? malloc((size_t)n) : NULL;
    if (u8)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, u8, n, NULL, NULL);
    return u8;
}

void win_make_parents(wchar_t *path)
{
    for (wchar_t *p = path + 1; *p; p++)
        if ((*p == L'\\' || *p == L'/') && p[-1] != L':' && p[-1] != L'\\' && p[-1] != L'/')
        {
            wchar_t c = *p;
            *p = 0;
            CreateDirectoryW(path, NULL); /* the open that follows reports what failed */
            *p = c;
        }
}

/* A path in full, resolved the way Win32 resolves one: a relative path against
 * the process working directory, and a drive-relative one ("C:") against the
 * directory Win32 stores for that drive. For "." at a drive root, the
 * sizing call reports room for "C:" only, and a buffer a few units long
 * receives "C:" in place of "C:\" with no error, so the first try uses a
 * MAX_PATH buffer, as .NET does. A longer path is tried again at the size
 * the call reports, which includes the terminating null. */
wchar_t *win_full_path(const wchar_t *w, api_errno *err)
{
    DWORD size = MAX_PATH;
    for (;;)
    {
        wchar_t *full = malloc((size_t)size * sizeof *full);
        if (!full)
        {
            *err = API_ENOMEM;
            return NULL;
        }
        DWORD got = GetFullPathNameW(w, size, full, NULL);
        if (got && got < size)
            return full;
        free(full);
        if (!got)
        {
            *err = win_last_error_to_api();
            return NULL;
        }
        size = got + 1; /* each try is larger, so the loop ends */
    }
}

/* NULL when no program could open the absolute path: one with a character the
 * code page cannot hold, or one on a UNC share, which a relative path resolves
 * to when the working directory is on one. A drive path in full starts with
 * "X:", and a UNC one with two backslashes. */
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
    char *out = wfull[1] == L':' && oem_maps_wide((const uint16_t *)wfull)
                    ? path_from_wide(wfull, &ignored)
                    : NULL;
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
 * uses, so the stamp is converted on the way. FileTimeToDosDateTime fails
 * outside 1980 to 2107, the only years a FAT date can hold, and the stamp is
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

/* A character with no byte in the running code page, or one that FAT refuses
 * in a name, shows as 0x7F, which no path may hold, so such an entry is listed
 * but cannot be opened. Win32 lists names with FAT's refused characters from
 * NTFS volumes and shares that other systems write. An 8.3 name never holds
 * one. */
static void info_from_find(f_stat_t *info, const WIN32_FIND_DATAW *fd)
{
    oem_from_wide((const uint16_t *)fd->cFileName, info->fname, sizeof info->fname);
    for (char *p = info->fname; *p; p++)
        if ((unsigned char)*p < 0x20 || strchr("\"*:<>?|\\", *p))
            *p = 0x7F;
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
    if (!path[0] || (win_has_drive(path) && !path[2]))
    {
        *err = path[0] && !win_drive_mounted(path[0]) ? API_ENODEV : API_EINVAL;
        return false;
    }
    wchar_t *w = path_to_wide(path, err);
    if (!w)
        return false;
    wchar_t *full = win_full_path(w, err);
    free(w);
    if (!full)
        return false;
    /* A find rather than GetFileAttributesEx, because only the find carries
     * the entry's own name in the case the volume stores, with the 8.3 name
     * beside it, which is what readdir reports and what stat has to agree
     * with. FindFirstFileW refuses a trailing separator, so it comes off, and
     * what is left of a root is "X:\", which has no entry to find. */
    size_t n = wcslen(full);
    while (n > 3 && full[n - 1] == L'\\')
        full[--n] = 0;
    bool ok;
    if (n == 3 && full[1] == L':')
    {
        /* Reading the attributes only checks that the volume is mounted. */
        ok = win_ok(GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES, err);
        if (ok)
            f_stat_root(info);
    }
    else
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(full, &fd);
        ok = win_ok(h != INVALID_HANDLE_VALUE, err);
        if (ok)
        {
            FindClose(h);
            info_from_find(info, &fd);
        }
    }
    free(full);
    return ok;
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
    char *abs = os_dir_realpath(path[0] ? path : ".");
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
        info_from_find(info, &d->fd);
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
    /* The API unlinks files and directories with one call. Anything not known
     * to be a directory goes to DeleteFileW, which reports a name that is not
     * there, and a read-only file as ERROR_ACCESS_DENIED. */
    DWORD a = GetFileAttributesW(w);
    bool ok = win_ok(a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)
                         ? RemoveDirectoryW(w)
                         : DeleteFileW(w),
                     err);
    free(w);
    return ok;
}

/* An existing entry at the new name is replaced only when both names are
 * files. The first attempt replaces nothing, so a change of case alone, which
 * Win32 matches to the same entry, still renames. */
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
    bool ok = MoveFileExW(wo, wn, 0);
    DWORD e = ok ? 0 : GetLastError();
    if (e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS)
    {
        DWORD ao = GetFileAttributesW(wo), an = GetFileAttributesW(wn);
        if (ao != INVALID_FILE_ATTRIBUTES && an != INVALID_FILE_ATTRIBUTES &&
            !((ao | an) & FILE_ATTRIBUTE_DIRECTORY))
        {
            ok = MoveFileExW(wo, wn, MOVEFILE_REPLACE_EXISTING);
            if (!ok)
                e = GetLastError();
        }
    }
    if (!ok)
        *err = win_error_to_api(e);
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

/* Win32 keeps one working directory for the whole process, and resolves a
 * bare "X:" against the hidden "=X:" environment variable, which cmd.exe keeps
 * and SetCurrentDirectoryW does not. The folder being left is written there
 * before every change, so each drive keeps the last folder used on it. */
static void win_note_cwd(void)
{
    api_errno ignored;
    wchar_t *w = win_full_path(L".", &ignored);
    if (w && w[1] == L':')
    {
        const wchar_t name[] = {L'=', (wchar_t)towupper(w[0]), L':', 0};
        SetEnvironmentVariableW(name, w);
    }
    free(w);
}

/* A path on another drive moves the process there, so a chdir switches drives
 * as it does on every machine. */
bool drive_chdir(const char *path, api_errno *err)
{
    if (!path[0])
        return true;
    wchar_t *w = path_to_wide(path, err);
    if (!w)
        return false;
    win_note_cwd();
    bool ok = win_ok(SetCurrentDirectoryW(w), err);
    free(w);
    return ok;
}

/* SetCurrentDirectoryW of a bare "X:" is the Windows change of drive, the one
 * cd /d and the CRT's _chdrive use. It changes to the folder in "=X:", or to
 * the drive's root when there is none. The letter is checked against the
 * mounted drives first, so a drive that is not there gives ENODEV rather than
 * a path error. */
bool drive_chdrive(const char *drive, api_errno *err)
{
    if (!drive[0])
        return true;
    if (!win_has_drive(drive) || drive[2] || !win_drive_mounted(drive[0]))
    {
        *err = API_ENODEV;
        return false;
    }
    win_note_cwd();
    const wchar_t w[3] = {(wchar_t)drive[0], L':', 0};
    return win_ok(SetCurrentDirectoryW(w), err);
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

/* A working directory on a UNC share has no drive letter, so no path a
 * program could write refers to it. */
bool drive_getcwd(char *buf, size_t size, api_errno *err)
{
    wchar_t *w = win_full_path(L".", err);
    if (!w)
        return false;
    bool ok = false;
    if (w[1] != L':')
        *err = API_ENODEV;
    else if (oem_from_wide((const uint16_t *)w, buf, size) >= size)
        *err = API_ENOMEM;
    else
    {
        win_to_slash(buf);
        ok = true;
    }
    free(w);
    return ok;
}

/* Only the Pico has volume labels, so the emulator never reads or renames a
 * host volume. */
bool drive_getlabel(const char *path, char *label, size_t size, api_errno *err)
{
    (void)path, (void)label, (void)size;
    *err = API_EACCES;
    return false;
}

bool drive_setlabel(const char *path, api_errno *err)
{
    (void)path;
    *err = API_EACCES;
    return false;
}

std_rw_result drive_getfree(const char *path, uint32_t *tot_sect, uint32_t *fre_sect,
                            api_errno *err)
{
    /* win_full_path settles a bare "C:" as that drive's own directory rather
     * than its root, the way every other call here reads one. */
    wchar_t *rel = path_to_wide(path[0] ? path : ".", err);
    if (!rel)
        return STD_ERROR;
    wchar_t *w = win_full_path(rel, err);
    free(rel);
    if (!w)
        return STD_ERROR;
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
        return STD_ERROR;
    uint64_t tot = total.QuadPart / 512;
    uint64_t fre = avail.QuadPart / 512;
    *tot_sect = tot > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)tot;
    *fre_sect = fre > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)fre;
    return STD_OK;
}

/* A Windows filesystem takes filenames as UTF-16, so there is no code page to
 * set. */
void oem_fs_code_page(uint16_t cp)
{
    (void)cp;
}

