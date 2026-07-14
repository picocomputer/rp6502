/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Shared Win32 seam helpers. See win/win.h.
 */

#include "emu/win/win.h"
#include <errno.h>

void win_set_errno(DWORD e)
{
    switch (e)
    {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_NO_MORE_FILES:
        errno = ENOENT;
        break;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        errno = EACCES;
        break;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        errno = EEXIST;
        break;
    case ERROR_NOT_ENOUGH_MEMORY:
        errno = ENOMEM;
        break;
    case ERROR_DIRECTORY:
        errno = ENOTDIR;
        break;
    case ERROR_DIR_NOT_EMPTY:
        errno = ENOTEMPTY;
        break;
    case ERROR_FILENAME_EXCED_RANGE:
        errno = ENAMETOOLONG;
        break;
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_HANDLE:
        errno = EINVAL;
        break;
    case ERROR_DISK_FULL:
        errno = ENOSPC;
        break;
    case ERROR_TOO_MANY_OPEN_FILES:
        errno = EMFILE;
        break;
    default:
        errno = EIO;
        break;
    }
}

void win_to_slash(char *p)
{
    for (; *p; p++)
        if (*p == '\\')
            *p = '/';
}
