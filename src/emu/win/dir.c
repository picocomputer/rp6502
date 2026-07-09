/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Windows directory enumeration (emu/plat.h dir_*), the Win32 counterpart of
 * posix/dir.c. SKELETON — fill the TODO(win) bodies.
 *
 * Paths cross the seam in the guest's OEM code page. Convert to UTF-16 with
 * oem_to_wide() (emu/api/oem.h) before every …W call, and convert returned names
 * back with oem_from_wide(). There is no opendir/readdir on Win32; use
 * FindFirstFileW/FindNextFileW/FindClose over an opaque heap struct.
 */

#include "emu/plat.h"
#include "emu/api/oem.h"
#include <errno.h>

void *dir_open(const char *path)
{
    (void)path;
    /* TODO(win): oem_to_wide(path) -> append L"\\*" -> FindFirstFileW. Heap a struct
     * holding { HANDLE h; WIN32_FIND_DATAW fd; bool first; wchar_t pattern[]; }.
     * FindFirstFileW already yields the first entry, so set first=true and stash it.
     * Return the struct, or NULL + errno (map GetLastError). */
    errno = ENOSYS;
    return NULL;
}

int dir_read(void *d, char *name, size_t namesz, bool *is_dir)
{
    (void)d, (void)name, (void)namesz, (void)is_dir;
    /* TODO(win): on the first call return the pending FindFirstFileW entry; thereafter
     * FindNextFileW. For each: *is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
     * oem_from_wide(fd.cFileName, name, namesz). Return 1 = entry, 0 = end
     * (GetLastError()==ERROR_NO_MORE_FILES), -1 + errno otherwise. */
    errno = ENOSYS;
    return -1;
}

void dir_rewind(void *d)
{
    (void)d;
    /* TODO(win): no rewind API — FindClose(h) then re-FindFirstFileW(stored pattern),
     * resetting first=true. */
}

void dir_close(void *d)
{
    (void)d;
    /* TODO(win): FindClose(h) if valid, then free the struct. */
}
