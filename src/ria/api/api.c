/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "api/api.h"
#include "sys/cpu.h"
#include "sys/ria.h"
#include "fatfs/ff.h"
#include <pico.h>

#if defined(DEBUG_RIA_API) || defined(DEBUG_RIA_API_API)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

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

// Logic to select the correct map
#define ENO_MAP(errno_name)                                                         \
    (eno_opt == 1) ? ENO_CC65_##errno_name : (eno_opt == 2) ? ENO_LLVM_##errno_name \
                                                            : 0;
static uint8_t eno_opt;
static uint8_t api_active_op;

void api_task(void)
{
    // Latch called op in case 6502 app misbehaves
    if (cpu_active() && !ria_active() &&
        !api_active_op && API_BUSY &&
        API_OP != 0x00 && API_OP != 0xFF)
        api_active_op = API_OP;
    if (api_active_op && !main_api(api_active_op))
        api_active_op = 0;
}

void api_run(void)
{
    eno_opt = ENO_OPT_NULL;
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

bool api_api_errno_opt(void)
{
    eno_opt = API_A;
    if (eno_opt != ENO_OPT_CC65 && eno_opt != ENO_OPT_LLVM)
        return api_return_errno(API_EINVAL);
    return api_return_ax(0);
}

uint16_t __in_flash("api_platform_errno") api_platform_errno(api_errno num)
{
    switch (num)
    {
    case API_ENOENT:
        return ENO_MAP(ENOENT);
    case API_ENOMEM:
        return ENO_MAP(ENOMEM);
    case API_EACCES:
        return ENO_MAP(EACCES);
    case API_ENODEV:
        return ENO_MAP(ENODEV);
    case API_EMFILE:
        return ENO_MAP(EMFILE);
    case API_EBUSY:
        return ENO_MAP(EBUSY);
    case API_EINVAL:
        return ENO_MAP(EINVAL);
    case API_ENOSPC:
        return ENO_MAP(ENOSPC);
    case API_EEXIST:
        return ENO_MAP(EEXIST);
    case API_EAGAIN:
        return ENO_MAP(EAGAIN);
    case API_EIO:
        return ENO_MAP(EIO);
    case API_EINTR:
        return ENO_MAP(EINTR);
    case API_ENOSYS:
        return ENO_MAP(ENOSYS);
    case API_ESPIPE:
        return ENO_MAP(ESPIPE);
    case API_ERANGE:
        return ENO_MAP(ERANGE);
    case API_EBADF:
        return ENO_MAP(EBADF);
    case API_ENOEXEC:
        return ENO_MAP(ENOEXEC);
    default:
        return ENO_MAP(EUNKNOWN);
    }
}

uint16_t __in_flash("api_fresult_errno") api_fresult_errno(unsigned fresult)
{
    switch ((FRESULT)fresult)
    {
    case FR_OK:
        return ENO_MAP(EUNKNOWN);
    case FR_DISK_ERR:
        return ENO_MAP(EUNKNOWN);
    case FR_INT_ERR:
        return ENO_MAP(EUNKNOWN);
    case FR_NOT_READY:
        return ENO_MAP(EUNKNOWN);
    case FR_NO_FILE:
        return ENO_MAP(EUNKNOWN);
    case FR_NO_PATH:
        return ENO_MAP(EUNKNOWN);
    case FR_INVALID_NAME:
        return ENO_MAP(EUNKNOWN);
    case FR_DENIED:
        return ENO_MAP(EUNKNOWN);
    case FR_EXIST:
        return ENO_MAP(EUNKNOWN);
    case FR_INVALID_OBJECT:
        return ENO_MAP(EUNKNOWN);
    case FR_WRITE_PROTECTED:
        return ENO_MAP(EUNKNOWN);
    case FR_INVALID_DRIVE:
        return ENO_MAP(EUNKNOWN);
    case FR_NOT_ENABLED:
        return ENO_MAP(EUNKNOWN);
    case FR_NO_FILESYSTEM:
        return ENO_MAP(EUNKNOWN);
    case FR_MKFS_ABORTED:
        return ENO_MAP(EUNKNOWN);
    case FR_TIMEOUT:
        return ENO_MAP(EUNKNOWN);
    case FR_LOCKED:
        return ENO_MAP(EUNKNOWN);
    case FR_NOT_ENOUGH_CORE:
        return ENO_MAP(EUNKNOWN);
    case FR_TOO_MANY_OPEN_FILES:
        return ENO_MAP(EUNKNOWN);
    case FR_INVALID_PARAMETER:
        return ENO_MAP(EUNKNOWN);
    default:
        return ENO_MAP(EUNKNOWN);
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
        memcpy((void *)data + 1, &xstack[xstack_ptr], sizeof(uint16_t) - 1);
        *data >>= 8 * 1;
        xstack_ptr = XSTACK_SIZE;
        return true;
    case XSTACK_SIZE - 2:
        memcpy((void *)data + 0, &xstack[xstack_ptr], sizeof(uint16_t) - 0);
        *data >>= 8 * 0;
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
        memcpy((void *)data + 3, &xstack[xstack_ptr], sizeof(uint32_t) - 3);
        *data >>= 8 * 3;
        xstack_ptr = XSTACK_SIZE;
        return true;
    case XSTACK_SIZE - 2:
        memcpy((void *)data + 2, &xstack[xstack_ptr], sizeof(uint32_t) - 2);
        *data >>= 8 * 2;
        xstack_ptr = XSTACK_SIZE;
        return true;
    case XSTACK_SIZE - 3:
        memcpy((void *)data + 1, &xstack[xstack_ptr], sizeof(uint32_t) - 1);
        *data >>= 8 * 1;
        xstack_ptr = XSTACK_SIZE;
        return true;
    case XSTACK_SIZE - 4:
        memcpy((void *)data + 0, &xstack[xstack_ptr], sizeof(uint32_t) - 0);
        *data >>= 8 * 0;
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
        memcpy((void *)data + 1, &xstack[xstack_ptr], sizeof(int16_t) - 1);
        *data >>= 8 * 1;
        xstack_ptr = XSTACK_SIZE;
        return true;
    case XSTACK_SIZE - 2:
        memcpy((void *)data + 0, &xstack[xstack_ptr], sizeof(int16_t) - 0);
        *data >>= 8 * 0;
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
        memcpy((void *)data + 3, &xstack[xstack_ptr], sizeof(int32_t) - 3);
        *data >>= 8 * 3;
        xstack_ptr = XSTACK_SIZE;
        return true;
    case XSTACK_SIZE - 2:
        memcpy((void *)data + 2, &xstack[xstack_ptr], sizeof(int32_t) - 2);
        *data >>= 8 * 2;
        xstack_ptr = XSTACK_SIZE;
        return true;
    case XSTACK_SIZE - 3:
        memcpy((void *)data + 1, &xstack[xstack_ptr], sizeof(int32_t) - 1);
        *data >>= 8 * 1;
        xstack_ptr = XSTACK_SIZE;
        return true;
    case XSTACK_SIZE - 4:
        memcpy((void *)data + 0, &xstack[xstack_ptr], sizeof(int32_t) - 0);
        *data >>= 8 * 0;
        xstack_ptr = XSTACK_SIZE;
        return true;
    default:
        return false;
    }
}
