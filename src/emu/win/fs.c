/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Windows filesystem metadata/namespace primitives (emu/plat.h fs_*), the Win32
 * counterpart of posix/fs.c. SKELETON — fill the TODO(win) bodies.
 *
 * Paths cross the seam in the guest's OEM code page. Convert to UTF-16 with
 * oem_to_wide() (emu/api/oem.h) before every …W call, and returned names/paths back
 * with oem_from_wide(). Fallible calls set errno and return false so the
 * msc_errno_to_api_errno funnel in api/hostfs.c works unchanged; a stub sets ENOSYS.
 *
 * Semantic decisions vs. POSIX (do NOT silently diverge):
 *   - is_hidden reads FILE_ATTRIBUTE_HIDDEN, not a leading-dot name.
 *   - crtime is the real ftCreationTime (POSIX has none; it reports change time).
 *   - fs_rename MUST replace an existing target (MOVEFILE_REPLACE_EXISTING).
 *   - fs_remove MUST also delete an empty directory (POSIX remove() does).
 *   - open the CRT in binary mode elsewhere (_O_BINARY) — not relevant here.
 */

#include "emu/plat.h"
#include "emu/api/oem.h"
#include <errno.h>
#include <string.h>
#include <time.h>

bool fs_stat(const char *path, struct fs_meta *out)
{
    (void)path, (void)out;
    /* TODO(win): GetFileAttributesExW(GetFileExInfoStandard) (or _wstat64). Fill:
     *   is_dir      = attrs & FILE_ATTRIBUTE_DIRECTORY
     *   is_readonly = attrs & FILE_ATTRIBUTE_READONLY
     *   is_hidden   = attrs & FILE_ATTRIBUTE_HIDDEN
     *   size        = ((uint64_t)nFileSizeHigh << 32) | nFileSizeLow
     *   mtime       = filetime_to_time_t(ftLastWriteTime)
     *   crtime      = filetime_to_time_t(ftCreationTime) */
    errno = ENOSYS;
    return false;
}

bool fs_freespace(const char *path, uint64_t *total_bytes, uint64_t *avail_bytes)
{
    (void)path, (void)total_bytes, (void)avail_bytes;
    /* TODO(win): GetDiskFreeSpaceExW(dir, &avail, &total, NULL) — already in bytes;
     * api/hostfs.c divides by 512. Pass the directory of the path, not the file. */
    errno = ENOSYS;
    return false;
}

bool fs_set_readonly(const char *path, bool readonly)
{
    (void)path, (void)readonly;
    /* TODO(win): a = GetFileAttributesW(w); set/clear FILE_ATTRIBUTE_READONLY;
     * SetFileAttributesW(w, a). */
    errno = ENOSYS;
    return false;
}

bool fs_set_mtime(const char *path, time_t mtime)
{
    (void)path, (void)mtime;
    /* TODO(win): time_t -> FILETIME (UTC); CreateFileW(FILE_WRITE_ATTRIBUTES) +
     * SetFileTime(h, NULL, NULL, &ft), or _wutime64. Set last-modified only. */
    errno = ENOSYS;
    return false;
}

bool fs_mkdir(const char *path)
{
    (void)path;
    /* TODO(win): _wmkdir(oem_to_wide(path)) — one argument on Windows. */
    errno = ENOSYS;
    return false;
}

bool fs_chdir(const char *path)
{
    (void)path;
    /* TODO(win): _wchdir(oem_to_wide(path)). */
    errno = ENOSYS;
    return false;
}

bool fs_getcwd(char *buf, size_t sz)
{
    (void)buf, (void)sz;
    /* TODO(win): _wgetcwd -> oem_from_wide into buf; rewrite '\\' -> '/'. */
    errno = ENOSYS;
    return false;
}

bool fs_rename(const char *oldp, const char *newp)
{
    (void)oldp, (void)newp;
    /* TODO(win): MoveFileExW(w_old, w_new, MOVEFILE_REPLACE_EXISTING) to match POSIX. */
    errno = ENOSYS;
    return false;
}

bool fs_remove(const char *path)
{
    (void)path;
    /* TODO(win): _wunlink(w); if it fails because the target is a directory, fall back
     * to _wrmdir(w) — POSIX remove() deletes a file or an empty directory. */
    errno = ENOSYS;
    return false;
}

void fs_localtime(time_t t, struct tm *out)
{
    localtime_s(out, &t); /* note: args reversed vs. POSIX localtime_r */
}

int fs_strcasecmp(const char *a, const char *b)
{
    return _stricmp(a, b);
}
