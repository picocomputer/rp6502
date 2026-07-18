/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Test helpers to drive the stdio syscall handlers (the vendored ria/api/std.c
 * std_api_*) the way the 6502 does: stage the args on the xstack / in API_A,
 * dispatch the handler until it stops working, then read the AX result and any
 * bytes it left on the xstack. Complements dirsys.h.
 */

#ifndef _EMU_TESTS_STDSYS_H_
#define _EMU_TESTS_STDSYS_H_

#include "api/api.h"
#include "api/std.h"
#include "emu/sys/mem.h"
#include "dirsys.h"
#include <stdint.h>
#include <string.h>

/* Dispatch a handler to completion: true means still working (async I/O,
 * chunked xram, a PIX-gated drain), which the machine re-dispatches every
 * scanline with std_task() alongside — mirror that pump here. */
static inline void ssys_dispatch(bool (*handler)(void))
{
    while (handler())
        std_task();
}

/* The platform errno a failed handler left (API_ERRNO). Decodable only after
 * api_set_errno_opt selects a mapping; compare against api_platform_errno(). */
static inline uint16_t ssys_errno(void)
{
    return API_ERRNO;
}

/* open(path, flags) -> fd, or -1. */
static inline int ssys_open(const char *path, uint8_t flags)
{
    dsys_path(path);
    API_A = flags;
    ssys_dispatch(std_api_open);
    return dsys_ax();
}

/* close(fd) -> 0, or -1. */
static inline int ssys_close(int fd)
{
    API_A = (uint8_t)fd;
    xstack_ptr = XSTACK_SIZE;
    ssys_dispatch(std_api_close);
    return dsys_ax();
}

/* read(fd, buf, n) -> bytes read (copied off the xstack), or -1. */
static inline int ssys_read(int fd, void *buf, uint16_t n)
{
    xstack_ptr = XSTACK_SIZE - 2;
    memcpy(&xstack[xstack_ptr], &n, 2);
    API_A = (uint8_t)fd;
    ssys_dispatch(std_api_read_xstack);
    int16_t ax = dsys_ax();
    if (ax > 0)
        memcpy(buf, &xstack[xstack_ptr], (size_t)ax);
    xstack_ptr = XSTACK_SIZE; /* consume the result like the 6502 pops it */
    return ax;
}

/* write(fd, buf, n) -> bytes written, or -1. */
static inline int ssys_write(int fd, const void *buf, uint16_t n)
{
    xstack_ptr = XSTACK_SIZE - n;
    memcpy(&xstack[xstack_ptr], buf, n);
    API_A = (uint8_t)fd;
    ssys_dispatch(std_api_write_xstack);
    return dsys_ax();
}

/* readx(fd, xram addr, n) -> bytes read into xram[addr], or -1. */
static inline int ssys_read_xram(int fd, uint16_t addr, uint16_t n)
{
    xstack_ptr = XSTACK_SIZE - 4;
    memcpy(&xstack[xstack_ptr], &n, 2);
    memcpy(&xstack[xstack_ptr + 2], &addr, 2);
    API_A = (uint8_t)fd;
    ssys_dispatch(std_api_read_xram);
    return dsys_ax();
}

/* writex(fd, xram addr, n) -> bytes written from xram[addr], or -1. */
static inline int ssys_write_xram(int fd, uint16_t addr, uint16_t n)
{
    xstack_ptr = XSTACK_SIZE - 4;
    memcpy(&xstack[xstack_ptr], &n, 2);
    memcpy(&xstack[xstack_ptr + 2], &addr, 2);
    API_A = (uint8_t)fd;
    ssys_dispatch(std_api_write_xram);
    return dsys_ax();
}

/* lseek(fd, ofs, whence) with POSIX whence -> new position, or -1. */
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
