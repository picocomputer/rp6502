/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _EMU_TESTS_STDSYS_H_
#define _EMU_TESTS_STDSYS_H_

#include "core/api/api.h"
#include "core/api/std.h"
#include "core/ria/regs.h"
#include "core/sys/xram.h"
#include "dirsys.h"
#include <stdint.h>
#include <string.h>

static inline void ssys_dispatch(bool (*handler)(void))
{
    while (handler())
        std_task();
}

/* API_ERRNO reads 0xFFFF for every error until api_set_errno_opt selects a
 * mapping, so a comparison against api_platform_errno() distinguishes errors
 * only after a mapping is selected. */
static inline uint16_t ssys_errno(void)
{
    return API_ERRNO;
}

static inline int ssys_open(const char *path, uint8_t flags)
{
    dsys_path(path);
    API_A = flags;
    ssys_dispatch(std_api_open);
    return dsys_ax();
}

static inline int ssys_close(int fd)
{
    API_A = (uint8_t)fd;
    xstack_ptr = XSTACK_SIZE;
    ssys_dispatch(std_api_close);
    return dsys_ax();
}

static inline int ssys_read(int fd, void *buf, uint16_t n)
{
    xstack_ptr = XSTACK_SIZE - 2;
    memcpy(&xstack[xstack_ptr], &n, 2);
    API_A = (uint8_t)fd;
    ssys_dispatch(std_api_read_xstack);
    int16_t ax = dsys_ax();
    if (ax > 0)
        memcpy(buf, &xstack[xstack_ptr], (size_t)ax);
    xstack_ptr = XSTACK_SIZE;
    return ax;
}

static inline int ssys_write(int fd, const void *buf, uint16_t n)
{
    xstack_ptr = XSTACK_SIZE - n;
    memcpy(&xstack[xstack_ptr], buf, n);
    API_A = (uint8_t)fd;
    ssys_dispatch(std_api_write_xstack);
    return dsys_ax();
}

static inline int ssys_read_xram(int fd, uint16_t addr, uint16_t n)
{
    xstack_ptr = XSTACK_SIZE - 4;
    memcpy(&xstack[xstack_ptr], &n, 2);
    memcpy(&xstack[xstack_ptr + 2], &addr, 2);
    API_A = (uint8_t)fd;
    ssys_dispatch(std_api_read_xram);
    return dsys_ax();
}

static inline int ssys_write_xram(int fd, uint16_t addr, uint16_t n)
{
    xstack_ptr = XSTACK_SIZE - 4;
    memcpy(&xstack[xstack_ptr], &n, 2);
    memcpy(&xstack[xstack_ptr + 2], &addr, 2);
    API_A = (uint8_t)fd;
    ssys_dispatch(std_api_write_xram);
    return dsys_ax();
}

static inline int32_t ssys_lseek(int fd, int32_t ofs, int8_t whence)
{
    xstack_ptr = XSTACK_SIZE - 5;
    xstack[xstack_ptr] = (uint8_t)whence;
    memcpy(&xstack[xstack_ptr + 1], &ofs, 4);
    API_A = (uint8_t)fd;
    ssys_dispatch(std_api_lseek_llvm);
    return dsys_axsreg();
}

#endif /* _EMU_TESTS_STDSYS_H_ */
