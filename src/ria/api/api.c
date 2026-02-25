/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "api/api.h"
#include "sys/cpu.h"
#include "sys/lfs.h"
#include "sys/ria.h"
#include <fatfs/ff.h>
#include <pico.h>

#if defined(DEBUG_RIA_API) || defined(DEBUG_RIA_API_API)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// These are known to both cc65 and llvm-mos
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

// llvm-mos supports these but cc65 doesn't
#define API_CC65_EDOM API_CC65_EUNKNOWN
#define API_LLVM_EDOM 33
#define API_CC65_EILSEQ API_CC65_EUNKNOWN
#define API_LLVM_EILSEQ 84

// Selected runtime option
#define API_ERRNO_OPT_NULL 0
#define API_ERRNO_OPT_CC65 1
#define API_ERRNO_OPT_LLVM 2

// Logic to select the platform errno map.
// The original plan was to default to 0 (don't change) until the
// errno option is set. Unfortunately, old cc65-compiled binaries
// use errno to detect stdio failures so we're defaulting to -1.
#define API_MAP(errno_name)                  \
    ((api_errno_opt == API_ERRNO_OPT_CC65)   \
         ? API_CC65_##errno_name             \
     : (api_errno_opt == API_ERRNO_OPT_LLVM) \
         ? API_LLVM_##errno_name             \
         : -1)

// API state
static uint8_t api_errno_opt;
static uint8_t api_active_op;

void api_task(void)
{
    // Latch called op in case 6502 app misbehaves
    if (cpu_active() && !ria_active() &&
        !api_active_op && API_BUSY)
    {
        uint8_t op = API_OP;
        if (op != 0x00 && op != 0xFF)
            api_active_op = op;
    }
    if (api_active_op && !main_api(api_active_op))
        api_active_op = 0;
}

void api_run(void)
{
    api_errno_opt = API_ERRNO_OPT_NULL;
    // All registers reset to a known state
    for (int i = 0; i < 16; i++)
        if (i != 3) // Skip VSYNC
            REGS(i) = 0;
    *(int8_t *)&REGS(0xFFE5) = 1; // STEP0
    REGS(0xFFE4) = xram[0];       // RW0
    *(int8_t *)&REGS(0xFFE9) = 1; // STEP1
    REGS(0xFFE8) = xram[0];       // RW1
    api_return_errno(0);
}

void api_stop(void)
{
    api_active_op = 0;
}

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

uint16_t __in_flash("api_platform_errno") api_platform_errno(api_errno num)
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

api_errno __in_flash("api_errno_from_fatfs") api_errno_from_fatfs(unsigned fresult)
{
    switch ((FRESULT)fresult)
    {
    case FR_DISK_ERR:
    case FR_INT_ERR:
    case FR_MKFS_ABORTED:
        return API_EIO;
    case FR_NOT_READY:
    case FR_INVALID_DRIVE:
    case FR_NOT_ENABLED:
    case FR_NO_FILESYSTEM:
        return API_ENODEV;
    case FR_NO_FILE:
    case FR_NO_PATH:
        return API_ENOENT;
    case FR_INVALID_NAME:
    case FR_INVALID_PARAMETER:
        return API_EINVAL;
    case FR_DENIED:
    case FR_WRITE_PROTECTED:
        return API_EACCES;
    case FR_EXIST:
        return API_EEXIST;
    case FR_INVALID_OBJECT:
        return API_EBADF;
    case FR_TIMEOUT:
        return API_EAGAIN;
    case FR_LOCKED:
        return API_EBUSY;
    case FR_NOT_ENOUGH_CORE:
        return API_ENOMEM;
    case FR_TOO_MANY_OPEN_FILES:
        return API_EMFILE;
    default:
        assert(false); // internal error
        return API_EIO;
    }
}

api_errno __in_flash("api_errno_from_lfs") api_errno_from_lfs(int lfs_err)
{
    switch (lfs_err)
    {
    case LFS_ERR_IO:
    case LFS_ERR_CORRUPT:
    case LFS_ERR_NOATTR:
        return API_EIO;
    case LFS_ERR_NOENT:
        return API_ENOENT;
    case LFS_ERR_EXIST:
        return API_EEXIST;
    case LFS_ERR_NOTDIR:
    case LFS_ERR_ISDIR:
    case LFS_ERR_NOTEMPTY:
    case LFS_ERR_INVAL:
    case LFS_ERR_NAMETOOLONG:
        return API_EINVAL;
    case LFS_ERR_BADF:
        return API_EBADF;
    case LFS_ERR_FBIG:
    case LFS_ERR_NOSPC:
        return API_ENOSPC;
    case LFS_ERR_NOMEM:
        return API_ENOMEM;
    default:
        return API_EIO;
    }
}

bool api_pop_uint8_end(uint8_t *data)
{
    switch (xstack_ptr)
    {
    case XSTACK_SIZE - 0:
        *data = 0;
        return true;
    case XSTACK_SIZE - 1:
        memcpy((void *)data, &xstack[xstack_ptr], sizeof(uint8_t));
        xstack_ptr = XSTACK_SIZE;
        return true;
    default:
        return false;
    }
}

bool api_pop_uint16_end(uint16_t *data)
{
    switch (xstack_ptr)
    {
    case XSTACK_SIZE - 0:
        *data = 0;
        return true;
    case XSTACK_SIZE - 1:
        *data = 0;
        memcpy(data, &xstack[xstack_ptr], 1);
        xstack_ptr = XSTACK_SIZE;
        return true;
    case XSTACK_SIZE - 2:
        memcpy(data, &xstack[xstack_ptr], 2);
        xstack_ptr = XSTACK_SIZE;
        return true;
    default:
        return false;
    }
}

bool api_pop_uint32_end(uint32_t *data)
{
    switch (xstack_ptr)
    {
    case XSTACK_SIZE - 0:
        *data = 0;
        return true;
    case XSTACK_SIZE - 1:
        *data = 0;
        memcpy(data, &xstack[xstack_ptr], 1);
        xstack_ptr = XSTACK_SIZE;
        return true;
    case XSTACK_SIZE - 2:
        *data = 0;
        memcpy(data, &xstack[xstack_ptr], 2);
        xstack_ptr = XSTACK_SIZE;
        return true;
    case XSTACK_SIZE - 3:
        *data = 0;
        memcpy(data, &xstack[xstack_ptr], 3);
        xstack_ptr = XSTACK_SIZE;
        return true;
    case XSTACK_SIZE - 4:
        memcpy(data, &xstack[xstack_ptr], 4);
        xstack_ptr = XSTACK_SIZE;
        return true;
    default:
        return false;
    }
}

bool api_pop_int8_end(int8_t *data)
{
    switch (xstack_ptr)
    {
    case XSTACK_SIZE - 0:
        *data = 0;
        return true;
    case XSTACK_SIZE - 1:
        memcpy((void *)data, &xstack[xstack_ptr], sizeof(int8_t));
        xstack_ptr = XSTACK_SIZE;
        return true;
    default:
        return false;
    }
}

bool api_pop_int16_end(int16_t *data)
{
    switch (xstack_ptr)
    {
    case XSTACK_SIZE - 0:
        *data = 0;
        return true;
    case XSTACK_SIZE - 1:
        *data = (int16_t)(int8_t)xstack[xstack_ptr];
        xstack_ptr = XSTACK_SIZE;
        return true;
    case XSTACK_SIZE - 2:
        memcpy(data, &xstack[xstack_ptr], 2);
        xstack_ptr = XSTACK_SIZE;
        return true;
    default:
        return false;
    }
}

bool api_pop_int32_end(int32_t *data)
{
    switch (xstack_ptr)
    {
    case XSTACK_SIZE - 0:
        *data = 0;
        return true;
    case XSTACK_SIZE - 1:
        *data = (int32_t)(int8_t)xstack[xstack_ptr];
        xstack_ptr = XSTACK_SIZE;
        return true;
    case XSTACK_SIZE - 2:
    {
        int16_t tmp;
        memcpy(&tmp, &xstack[xstack_ptr], 2);
        *data = (int32_t)tmp;
        xstack_ptr = XSTACK_SIZE;
        return true;
    }
    case XSTACK_SIZE - 3:
        *data = 0;
        memcpy(data, &xstack[xstack_ptr], 3);
        *data = (*data ^ 0x00800000) - 0x00800000;
        xstack_ptr = XSTACK_SIZE;
        return true;
    case XSTACK_SIZE - 4:
        memcpy(data, &xstack[xstack_ptr], 4);
        xstack_ptr = XSTACK_SIZE;
        return true;
    default:
        return false;
    }
}
