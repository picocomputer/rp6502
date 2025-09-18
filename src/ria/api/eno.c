/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/eno.h"
#include <errno.h>

// These are known to both cc65 and llvm-mos
#define ENO_CC65_ENOENT 1 /* No such file or directory */
#define ENO_LLVM_ENOENT 2
#define ENO_CC65_ENOMEM 2 /* Out of memory */
#define ENO_LLVM_ENOMEM 12
#define ENO_CC65_EACCES 3 /* Permission denied */
#define ENO_LLVM_EACCES 13
#define ENO_CC65_ENODEV 4 /* No such device */
#define ENO_LLVM_ENODEV 19
#define ENO_CC65_EMFILE 5 /* Too many open files */
#define ENO_LLVM_EMFILE 24
#define ENO_CC65_EBUSY 6 /* Device or resource busy */
#define ENO_LLVM_EBUSY 16
#define ENO_CC65_EINVAL 7 /* Invalid argument */
#define ENO_LLVM_EINVAL 22
#define ENO_CC65_ENOSPC 8 /* No space left on device */
#define ENO_LLVM_ENOSPC 28
#define ENO_CC65_EEXIST 9 /* File exists */
#define ENO_LLVM_EEXIST 17
#define ENO_CC65_EAGAIN 10 /* Try again */
#define ENO_LLVM_EAGAIN 11
#define ENO_CC65_EIO 11 /* I/O error */
#define ENO_LLVM_EIO 5
#define ENO_CC65_EINTR 12 /* Interrupted system call */
#define ENO_LLVM_EINTR 4
#define ENO_CC65_ENOSYS 13 /* Function not implemented */
#define ENO_LLVM_ENOSYS 38
#define ENO_CC65_ESPIPE 14 /* Illegal seek */
#define ENO_LLVM_ESPIPE 29
#define ENO_CC65_ERANGE 15 /* Range error */
#define ENO_LLVM_ERANGE 34
#define ENO_CC65_EBADF 16 /* Bad file number */
#define ENO_LLVM_EBADF 9
#define ENO_CC65_ENOEXEC 17 /* Exec format error */
#define ENO_LLVM_ENOEXEC 8
#define ENO_CC65_EUNKNOWN 18 /* Unknown OS specific error */
#define ENO_LLVM_EUNKNOWN 85

// llvm-mos uniques
#define ENO_LLVM_EDOM 33
#define ENO_LLVM_EILSEQ 84

void eno_run(void)
{
}

bool eno_api_errno_opt(void)
{
    return false;
}

uint16_t eno_posix(unsigned num)
{
    return 0;
}

uint16_t eno_fatfs(unsigned fresult)
{
    return 0;
}
