/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_API_ENO_H_
#define _RIA_API_ENO_H_

/* This allows selection of retured errno because
 * cc65 and llvm-mos have different errno.h constants.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum
{
    ENO_ENOENT,  /* No such file or directory */
    ENO_ENOMEM,  /* Out of memory */
    ENO_EACCES,  /* Permission denied */
    ENO_ENODEV,  /* No such device */
    ENO_EMFILE,  /* Too many open files */
    ENO_EBUSY,   /* Device or resource busy */
    ENO_EINVAL,  /* Invalid argument */
    ENO_ENOSPC,  /* No space left on device */
    ENO_EEXIST,  /* File exists */
    ENO_EAGAIN,  /* Try again */
    ENO_EIO,     /* I/O error */
    ENO_EINTR,   /* Interrupted system call */
    ENO_ENOSYS,  /* Function not implemented */
    ENO_ESPIPE,  /* Illegal seek */
    ENO_ERANGE,  /* Range error */
    ENO_EBADF,   /* Bad file number */
    ENO_ENOEXEC, /* Exec format error */
    // ENO_EUNKNOWN /* Unknown error */
} eno_errno;

void eno_run(void);
bool eno_api_errno_opt(void);

uint16_t eno_posix(eno_errno num);
uint16_t eno_fatfs(unsigned fresult);

#endif /* _RIA_API_ENO_H_ */
