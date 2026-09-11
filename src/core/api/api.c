/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/ria.h"
#include "core/api/ops.h"
#include "core/sys/driver.h"
#include "core/api/api.h"
#include "core/wdc/resb.h"

/* Both toolchains define these errno values, except EUNKNOWN at the end of
 * the list, which is this OS's own catch-all number. */
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

// llvm-mos defines these but cc65 does not, so cc65 maps them to EUNKNOWN.
#define API_CC65_EDOM API_CC65_EUNKNOWN
#define API_LLVM_EDOM 33
#define API_CC65_EILSEQ API_CC65_EUNKNOWN
#define API_LLVM_EILSEQ 84

#define API_ERRNO_OPT_NULL 0
#define API_ERRNO_OPT_CC65 1
#define API_ERRNO_OPT_LLVM 2

// Until a program selects a map, every errno becomes -1, because old
// cc65-compiled binaries read errno to detect a stdio failure and leaving it
// unchanged would hide one.
#define API_MAP(errno_name)                  \
    ((api_errno_opt == API_ERRNO_OPT_CC65)   \
         ? API_CC65_##errno_name             \
     : (api_errno_opt == API_ERRNO_OPT_LLVM) \
         ? API_LLVM_##errno_name             \
         : -1)

static uint8_t api_errno_opt;
static uint8_t api_active_op;

// The op number is copied because the 6502 can write API_OP again while an op
// is in flight. Ops 0x00 and 0xFF are never latched, because each machine
// answers those itself where the 6502 writes them.
void api_task(void)
{
    if (resb_running() && !ria_active() &&
        !api_active_op && API_BUSY)
    {
        uint8_t op = API_OP;
        if (op != 0x00 && op != 0xFF)
            api_active_op = op;
    }
    if (api_active_op && !ops_dispatch(api_active_op))
        api_active_op = 0;
}

void api_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put_u8(c, api_active_op);
    sst_put_u8(c, api_errno_opt);
}

bool api_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    uint8_t op = sst_get_u8(c);
    uint8_t opt = sst_get_u8(c);
    if (!sst_ok(c))
        return false;
    api_active_op = op;
    api_errno_opt = opt;
    return true;
}

void api_stop(void)
{
    api_active_op = 0;
}

void api_run(void)
{
    /* A fast load cycles RESB and so reaches api_run without a program
     * starting. Nothing below applies to one, and the return registers written
     * below overlap $FFF2-$FFF7 of the self-modifying stub the RIA is driving
     * the 6502 with. */
    if (ria_active())
        return;
    api_errno_opt = API_ERRNO_OPT_NULL;
    // $FFE3 is skipped because it is the VSYNC frame counter, which vga owns.
    for (int addr = 0xFFE0; addr <= 0xFFEF; addr++)
        if (addr != 0xFFE3)
            REGS(addr) = 0;
    xstack_ptr = XSTACK_SIZE;
    REGS(0xFFE5) = 1; // STEP0
    REGS(0xFFE9) = 1; // STEP1
    API_ERRNO = 0xFFFF;
    api_set_axsreg(-1);
    api_set_regs_released();
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

// The sign bit is in xstack[XSTACK_SIZE - 1] because this pop must empty the
// stack, so that byte is the value's most significant.
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

bool api_pop_uint8_end(uint8_t *data)
{
    return api_pop_end(data, sizeof(*data), false);
}

bool api_pop_uint16_end(uint16_t *data)
{
    return api_pop_end(data, sizeof(*data), false);
}

bool api_pop_uint32_end(uint32_t *data)
{
    return api_pop_end(data, sizeof(*data), false);
}

bool api_pop_uint64_end(uint64_t *data)
{
    return api_pop_end(data, sizeof(*data), false);
}

bool api_pop_int8_end(int8_t *data)
{
    return api_pop_end(data, sizeof(*data), true);
}

bool api_pop_int16_end(int16_t *data)
{
    return api_pop_end(data, sizeof(*data), true);
}

bool api_pop_int32_end(int32_t *data)
{
    return api_pop_end(data, sizeof(*data), true);
}
