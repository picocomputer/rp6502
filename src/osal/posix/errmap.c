/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See errno.h.
 */

#include "osal/posix/errmap.h"
#include <errno.h>

api_errno errno_to_api(int host_errno)
{
    switch (host_errno)
    {
    case ENOENT:
    /* FatFs spells a path that runs through a file FR_NO_PATH, which is
     * this, and api_errno has no ENOTDIR to spell it any other way. */
    case ENOTDIR:
        return API_ENOENT;
    case EACCES:
    case EPERM:
    case EROFS:
    /* Both of these are FR_DENIED on FatFs: a directory opened as a file,
     * and a directory removed with something still in it. */
    case EISDIR:
    case ENOTEMPTY:
        return API_EACCES;
    case EEXIST:
        return API_EEXIST;
    case EINVAL:
    case ENAMETOOLONG:
        return API_EINVAL;
    case ENOSPC:
    case EFBIG:
        return API_ENOSPC;
    case EMFILE:
    case ENFILE:
        return API_EMFILE;
    case EBADF:
        return API_EBADF;
    case EBUSY:
        return API_EBUSY;
    case ENODEV:
    case ENXIO:
        return API_ENODEV;
    case EAGAIN:
        return API_EAGAIN;
    case ENOMEM:
        return API_ENOMEM;
    case ESPIPE:
        return API_ESPIPE;
    case ERANGE:
        return API_ERANGE;
    default:
        return API_EIO;
    }
}

api_errno errno_to_api_rw(int host_errno)
{
    return host_errno == EBADF ? API_EACCES : errno_to_api(host_errno);
}
