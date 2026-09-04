/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See errno.h.
 */

#include "osal/windows/errmap.h"

api_errno win_error_to_api(DWORD e)
{
    switch (e)
    {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_NO_MORE_FILES:
        return API_ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_WRITE_PROTECT:
        return API_EACCES;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        return API_EEXIST;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return API_ENOMEM;
    case ERROR_DIRECTORY:
    case ERROR_DIR_NOT_EMPTY:
    case ERROR_FILENAME_EXCED_RANGE:
    case ERROR_INVALID_PARAMETER:
    case ERROR_NEGATIVE_SEEK:
        return API_EINVAL;
    case ERROR_INVALID_HANDLE:
        return API_EBADF;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return API_ENOSPC;
    case ERROR_TOO_MANY_OPEN_FILES:
        return API_EMFILE;
    case ERROR_NOT_READY:
    case ERROR_BAD_UNIT:
    case ERROR_INVALID_DRIVE:
        return API_ENODEV;
    case ERROR_BUSY:
    case ERROR_PIPE_BUSY:
        return API_EBUSY;
    case ERROR_OPERATION_ABORTED:
        return API_EINTR;
    default:
        return API_EIO;
    }
}
