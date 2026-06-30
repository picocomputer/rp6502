/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The non-inline helpers that ria/api/api.h declares but does not define:
 * the errno-option machinery and the "short stack" pops. Ported from
 * ria/api/api.c (the FatFs/lfs error mappers are omitted — nothing the
 * emulator compiles calls api_return_fresult). This lets the firmware's
 * api.h, the std/dir handlers, and rln.c link against the emulator, and
 * supplies api_errno_from_host — the POSIX-errno mapper the host-backed
 * filesystem (std.c/dir.c) reports failures through.
 */

#include "emu/api/api.h"
#include "emu/sys/mem.h"
#include "api/api.h"
#include <errno.h>
#include <string.h>

/* cc65 and llvm-mos errno values (from api.c). */
#define API_CC65_ENOENT 1
#define API_LLVM_ENOENT 2
#define API_CC65_ENOMEM 2
#define API_LLVM_ENOMEM 12
#define API_CC65_EACCES 3
#define API_LLVM_EACCES 13
#define API_CC65_ENODEV 4
#define API_LLVM_ENODEV 19
#define API_CC65_EMFILE 5
#define API_LLVM_EMFILE 24
#define API_CC65_EBUSY 6
#define API_LLVM_EBUSY 16
#define API_CC65_EINVAL 7
#define API_LLVM_EINVAL 22
#define API_CC65_ENOSPC 8
#define API_LLVM_ENOSPC 28
#define API_CC65_EEXIST 9
#define API_LLVM_EEXIST 17
#define API_CC65_EAGAIN 10
#define API_LLVM_EAGAIN 11
#define API_CC65_EIO 11
#define API_LLVM_EIO 5
#define API_CC65_EINTR 12
#define API_LLVM_EINTR 4
#define API_CC65_ENOSYS 13
#define API_LLVM_ENOSYS 38
#define API_CC65_ESPIPE 14
#define API_LLVM_ESPIPE 29
#define API_CC65_ERANGE 15
#define API_LLVM_ERANGE 34
#define API_CC65_EBADF 16
#define API_LLVM_EBADF 9
#define API_CC65_ENOEXEC 17
#define API_LLVM_ENOEXEC 8
#define API_CC65_EUNKNOWN 18
#define API_LLVM_EUNKNOWN 85
#define API_CC65_EDOM API_CC65_EUNKNOWN
#define API_LLVM_EDOM 33
#define API_CC65_EILSEQ API_CC65_EUNKNOWN
#define API_LLVM_EILSEQ 84

#define API_ERRNO_OPT_NULL 0
#define API_ERRNO_OPT_CC65 1
#define API_ERRNO_OPT_LLVM 2

#define API_MAP(errno_name)                  \
    ((api_errno_opt == API_ERRNO_OPT_CC65)   \
         ? API_CC65_##errno_name             \
     : (api_errno_opt == API_ERRNO_OPT_LLVM) \
         ? API_LLVM_##errno_name             \
         : -1)

static uint8_t api_errno_opt;

uint8_t api_get_errno_opt(void)
{
    return api_errno_opt;
}

bool api_set_errno_opt(uint8_t opt)
{
    if (opt != API_ERRNO_OPT_CC65 && opt != API_ERRNO_OPT_LLVM)
        return false;
    api_errno_opt = opt;
    return true;
}

uint16_t api_platform_errno(api_errno num)
{
    switch (num)
    {
    case API_ENOENT:
        return API_MAP(ENOENT);
    case API_ENOMEM:
        return API_MAP(ENOMEM);
    case API_EACCES:
        return API_MAP(EACCES);
    case API_ENODEV:
        return API_MAP(ENODEV);
    case API_EMFILE:
        return API_MAP(EMFILE);
    case API_EBUSY:
        return API_MAP(EBUSY);
    case API_EINVAL:
        return API_MAP(EINVAL);
    case API_ENOSPC:
        return API_MAP(ENOSPC);
    case API_EEXIST:
        return API_MAP(EEXIST);
    case API_EAGAIN:
        return API_MAP(EAGAIN);
    case API_EIO:
        return API_MAP(EIO);
    case API_EINTR:
        return API_MAP(EINTR);
    case API_ENOSYS:
        return API_MAP(ENOSYS);
    case API_ESPIPE:
        return API_MAP(ESPIPE);
    case API_ERANGE:
        return API_MAP(ERANGE);
    case API_EBADF:
        return API_MAP(EBADF);
    case API_ENOEXEC:
        return API_MAP(ENOEXEC);
    case API_EDOM:
        return API_MAP(EDOM);
    case API_EILSEQ:
        return API_MAP(EILSEQ);
    default:
        return API_MAP(EUNKNOWN);
    }
}

/* "Short stack" final-argument pops: zero-fill or sign-extend to the full
 * width, requiring the stack to be empty afterward (from api.c). */
static bool api_pop_end(void *data, size_t size, bool sign)
{
    size_t n = XSTACK_SIZE - xstack_ptr;
    if (n > size)
        return false;
    int fill = (sign && n && (xstack[XSTACK_SIZE - 1] & 0x80)) ? 0xFF : 0;
    memset(data, fill, size);
    memcpy(data, &xstack[xstack_ptr], n);
    xstack_ptr = XSTACK_SIZE;
    return true;
}

bool api_pop_uint8_end(uint8_t *data) { return api_pop_end(data, sizeof(*data), false); }
bool api_pop_uint16_end(uint16_t *data) { return api_pop_end(data, sizeof(*data), false); }
bool api_pop_uint32_end(uint32_t *data) { return api_pop_end(data, sizeof(*data), false); }
bool api_pop_uint64_end(uint64_t *data) { return api_pop_end(data, sizeof(*data), false); }
bool api_pop_int8_end(int8_t *data) { return api_pop_end(data, sizeof(*data), true); }
bool api_pop_int16_end(int16_t *data) { return api_pop_end(data, sizeof(*data), true); }
bool api_pop_int32_end(int32_t *data) { return api_pop_end(data, sizeof(*data), true); }

/* The fs backends report failures by setting POSIX errno; translate to the 6502 set. */
api_errno api_errno_from_host(int host_errno)
{
    switch (host_errno)
    {
    case ENOENT:
        return API_ENOENT;
    case EACCES:
    case EPERM:
    case EROFS:
        return API_EACCES;
    case EEXIST:
        return API_EEXIST;
    case EINVAL:
    case EISDIR:
    case ENOTDIR:
    case ENOTEMPTY:
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
