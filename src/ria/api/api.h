/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_API_API_H_
#define _RIA_API_API_H_

/* The API driver manages function calls from the 6502.
 * This header includes helpers for API implementations.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "sys/mem.h"

/* Main events
 */

void api_task(void);
void api_run(void);
void api_stop(void);

typedef enum
{
    API_ENOENT,  /* No such file or directory */
    API_ENOMEM,  /* Not enough space */
    API_EACCES,  /* Permission denied */
    API_ENODEV,  /* No such device */
    API_EMFILE,  /* Too many open files */
    API_EBUSY,   /* Device or resource busy */
    API_EINVAL,  /* Invalid argument */
    API_ENOSPC,  /* No space left on device */
    API_EEXIST,  /* File exists */
    API_EAGAIN,  /* Resource unavailable, try again */
    API_EIO,     /* I/O error */
    API_EINTR,   /* Interrupted system call */
    API_ENOSYS,  /* Function not supported */
    API_ESPIPE,  /* Illegal seek */
    API_ERANGE,  /* Result too large */
    API_EBADF,   /* Bad file descriptor */
    API_ENOEXEC, /* Executable file format error */
    // The following are required for ISO C but cc65 doesn't
    // have them and so will map to its internal EUNKNOWN.
    API_EDOM,   /* Mathematics argument out of domain of function*/
    API_EILSEQ, /* Invalid or incomplete multibyte or wide character */
} api_errno;

// cc65 and llvm-mos C init calls this
// to select its errno.h constants.
bool api_api_errno_opt(void);

// Used by macros to turn an api_errno
// into a cc65 or llvm-mos errno.
uint16_t api_platform_errno(api_errno num);

// Used by macros to turn a FatFs FRESULT
// into a cc65 or llvm-mos errno.
uint16_t api_fresult_errno(unsigned fresult);

/* RIA fastcall registers
 */

#define API_OP REGS(0xFFEF)
#define API_ERRNO REGSW(0xFFED)
#define API_STACK REGS(0xFFEC)
#define API_BUSY (REGS(0xFFF2) & 0x80)
#define API_A REGS(0xFFF4)
#define API_X REGS(0xFFF6)
#define API_SREG REGSW(0xFFF8)
#define API_AX (API_A | (API_X << 8))
#define API_AXSREG (API_AX | (API_SREG << 16))

// How to build an API handler:
// 1. The last fastcall argument is in API_A, API_AX or API_AXSREG.
// 2. Stack was pushed "in order". Like any top-down stack.
// 3. First parameter supports a "short stack".
//    e.g. a uint16 is sent for fseek instead of a uint64.
// 4. Be careful with the stack. Use this API.
// 5. Registers must be refreshed if XSTACK data changes.
// 6. Use the return functions always!

/* The last stack value, which is the first argument on the CC65 side,
 * may be a "short stack" to keep 6502 code as small as possible.
 * These will fail if the stack would not be empty after the pop.
 */

bool api_pop_uint8_end(uint8_t *data);
bool api_pop_uint16_end(uint16_t *data);
bool api_pop_uint32_end(uint32_t *data);
bool api_pop_int8_end(int8_t *data);
bool api_pop_int16_end(int16_t *data);
bool api_pop_int32_end(int32_t *data);

// Safely pop n bytes off the xstack. Fails with false if will underflow.
static inline bool api_pop_n(void *data, size_t n)
{
    if (XSTACK_SIZE - xstack_ptr < n)
        return false;
    memcpy(data, &xstack[xstack_ptr], n);
    xstack_ptr += n;
    return true;
}

/* Ordinary xstack popping. Use these for all but the final argument.
 */

static inline bool api_pop_uint8(uint8_t *data) { return api_pop_n(data, sizeof(uint8_t)); }
static inline bool api_pop_uint16(uint16_t *data) { return api_pop_n(data, sizeof(uint16_t)); }
static inline bool api_pop_uint32(uint32_t *data) { return api_pop_n(data, sizeof(uint32_t)); }
static inline bool api_pop_int8(int8_t *data) { return api_pop_n(data, sizeof(int8_t)); }
static inline bool api_pop_int16(int16_t *data) { return api_pop_n(data, sizeof(int16_t)); }
static inline bool api_pop_int32(int32_t *data) { return api_pop_n(data, sizeof(int32_t)); }

// Safely push n bytes to the xstack. Fails with false if no room.
static inline bool api_push_n(const void *data, size_t n)
{
    if (n > xstack_ptr)
        return false;
    xstack_ptr -= n;
    memcpy(&xstack[xstack_ptr], data, n);
    return true;
}

/* Wrappers for ordinary xstack pushing.
 */

static inline bool api_push_uint8(const uint8_t *data) { return api_push_n(data, sizeof(uint8_t)); }
static inline bool api_push_uint16(const uint16_t *data) { return api_push_n(data, sizeof(uint16_t)); }
static inline bool api_push_uint32(const uint32_t *data) { return api_push_n(data, sizeof(uint32_t)); }
static inline bool api_push_int8(const int8_t *data) { return api_push_n(data, sizeof(int8_t)); }
static inline bool api_push_int16(const int16_t *data) { return api_push_n(data, sizeof(int16_t)); }
static inline bool api_push_int32(const int32_t *data) { return api_push_n(data, sizeof(int32_t)); }

// Return works by manipulating 10 bytes of registers.
// FFF0 EA      NOP
// FFF1 80 FE   BRA -2
// FFF3 A9 FF   LDA #$FF
// FFF5 A2 FF   LDX #$FF
// FFF7 60      RTS
// FFF8 FF FF   .SREG $FF $FF

static inline void api_set_regs_blocked() { *(uint32_t *)&regs[0x10] = 0xA9FE80EA; }
static inline void api_set_regs_released() { *(uint32_t *)&regs[0x10] = 0xA90080EA; }

/* Sets the return value along with the LDX and RTS.
 */

static inline void api_set_ax(uint16_t val)
{
    *(uint32_t *)&regs[0x14] = 0x6000A200 | (val & 0xFF) | ((val << 8) & 0xFF0000);
}

static inline void api_set_axsreg(uint32_t val)
{
    api_set_ax(val);
    API_SREG = val >> 16;
}

/* API workers must not block and must return one of these at the very end.
 */

// Return this if waiting on IO
static inline bool api_working(void)
{
    return true;
}

// Success for when api_set_ax has already been called.
static inline bool api_return(void)
{
    api_set_regs_released();
    API_STACK = xstack[xstack_ptr];
    return false;
}

// Success with a 16 bit return
static inline bool api_return_ax(uint16_t val)
{
    api_set_ax(val);
    return api_return();
}

// Success with a 32 bit return
static inline bool api_return_axsreg(uint32_t val)
{
    api_set_axsreg(val);
    return api_return();
}

// Failure returns -1 and sets errno
static inline bool api_return_errno(api_errno errno)
{
    uint16_t platform_errno = api_platform_errno(errno);
    if (platform_errno)
        API_ERRNO = platform_errno;
    xstack_ptr = XSTACK_SIZE;
    return api_return_axsreg(-1);
}

// Failure returns -1 and sets errno from FatFS FRESULT
static inline bool api_return_fresult(unsigned fresult)
{
    uint16_t platform_errno = api_fresult_errno(fresult);
    if (platform_errno)
        API_ERRNO = platform_errno;
    xstack_ptr = XSTACK_SIZE;
    return api_return_axsreg(-1);
}

#endif /* _RIA_API_API_H_ */
