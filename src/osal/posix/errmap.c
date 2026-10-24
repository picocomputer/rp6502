/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See errmap.h.
 */

#include "osal/posix/errmap.h"
#include <errno.h>

api_errno errno_to_api(int host_errno)
{
    switch (host_errno)
    {
    case ENOENT:
    /* A path that runs through a file is FR_NO_PATH on FatFs, which maps
     * here, and api_errno has no ENOTDIR of its own. */
    case ENOTDIR:
        return API_ENOENT;
    case EACCES:
    case EPERM:
    case EROFS:
    /* FatFs answers FR_DENIED to a directory opened as a file. */
    case EISDIR:
        return API_EACCES;
    case EEXIST:
    /* rmdir and rename give either one for a directory that is not empty,
     * and api_errno has no ENOTEMPTY. */
    case ENOTEMPTY:
        return API_EEXIST;
    case EINVAL:
    case ENAMETOOLONG:
        return API_EINVAL;
    case ENOSPC:
    case EFBIG:
    case EDQUOT:
        return API_ENOSPC;
    case EMFILE:
    case ENFILE:
        return API_EMFILE;
    case EBADF:
        return API_EBADF;
    case EBUSY:
    case ETXTBSY:
        return API_EBUSY;
    case ENODEV:
    case ENXIO:
    /* A rename across mounts is one across drives. */
    case EXDEV:
        return API_ENODEV;
    case EAGAIN:
        return API_EAGAIN;
    case EINTR:
        return API_EINTR;
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
