/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/eno.h"

// These are known to both cc65 and llvm-mos
#define ENO_CC65_ENOENT 1
#define ENO_LLVM_ENOENT 2
#define ENO_CC65_ENOMEM 2
#define ENO_LLVM_ENOMEM 12
#define ENO_CC65_EACCES 3
#define ENO_LLVM_EACCES 13
#define ENO_CC65_ENODEV 4
#define ENO_LLVM_ENODEV 19
#define ENO_CC65_EMFILE 5
#define ENO_LLVM_EMFILE 24
#define ENO_CC65_EBUSY 6
#define ENO_LLVM_EBUSY 16
#define ENO_CC65_EINVAL 7
#define ENO_LLVM_EINVAL 22
#define ENO_CC65_ENOSPC 8
#define ENO_LLVM_ENOSPC 28
#define ENO_CC65_EEXIST 9
#define ENO_LLVM_EEXIST 17
#define ENO_CC65_EAGAIN 10
#define ENO_LLVM_EAGAIN 11
#define ENO_CC65_EIO 11
#define ENO_LLVM_EIO 5
#define ENO_CC65_EINTR 12
#define ENO_LLVM_EINTR 4
#define ENO_CC65_ENOSYS 13
#define ENO_LLVM_ENOSYS 38
#define ENO_CC65_ESPIPE 14
#define ENO_LLVM_ESPIPE 29
#define ENO_CC65_ERANGE 15
#define ENO_LLVM_ERANGE 34
#define ENO_CC65_EBADF 16
#define ENO_LLVM_EBADF 9
#define ENO_CC65_ENOEXEC 17
#define ENO_LLVM_ENOEXEC 8
#define ENO_CC65_EUNKNOWN 18
#define ENO_LLVM_EUNKNOWN 85

// llvm-mos uniques, unused
#define ENO_LLVM_EDOM 33
#define ENO_LLVM_EILSEQ 84

// Supported runtime options
#define ENO_OPT_NULL 0
#define ENO_OPT_CC65 1
#define ENO_OPT_LLVM 2

static uint8_t eno_opt;

void eno_run(void)
{
    eno_opt = ENO_OPT_NULL;
}

bool eno_api_errno_opt(void)
{
    return false;
}

#define ENO_MAP(errno_name)                                                         \
    (eno_opt == 1) ? ENO_CC65_##errno_name : (eno_opt == 2) ? ENO_LLVM_##errno_name \
                                                            : 0;

uint16_t eno_posix(eno_errno num)
{
    switch (num)
    {
    case ENO_ENOENT:
        return ENO_MAP(ENOENT);
    case ENO_ENOMEM:
        return ENO_MAP(ENOMEM);
    case ENO_EACCES:
        return ENO_MAP(EACCES);
    case ENO_ENODEV:
        return ENO_MAP(ENODEV);
    case ENO_EMFILE:
        return ENO_MAP(EMFILE);
    case ENO_EBUSY:
        return ENO_MAP(EBUSY);
    case ENO_EINVAL:
        return ENO_MAP(EINVAL);
    case ENO_ENOSPC:
        return ENO_MAP(ENOSPC);
    case ENO_EEXIST:
        return ENO_MAP(EEXIST);
    case ENO_EAGAIN:
        return ENO_MAP(EAGAIN);
    case ENO_EIO:
        return ENO_MAP(EIO);
    case ENO_EINTR:
        return ENO_MAP(EINTR);
    case ENO_ENOSYS:
        return ENO_MAP(ENOSYS);
    case ENO_ESPIPE:
        return ENO_MAP(ESPIPE);
    case ENO_ERANGE:
        return ENO_MAP(ERANGE);
    case ENO_EBADF:
        return ENO_MAP(EBADF);
    case ENO_ENOEXEC:
        return ENO_MAP(ENOEXEC);
    case ENO_EUNKNOWN:
    default:
        return ENO_MAP(EUNKNOWN);
    }
}

uint16_t eno_fatfs(unsigned fresult)
{
    return 0;
}
