/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_API_API_H_
#define _CORE_API_API_H_

#include <stddef.h>
#include <stdint.h>
#include "core/sys/sst.h"
#include <stdbool.h>
#include <string.h>
#include "core/ria/regs.h"

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
    // ISO C requires the two below, but cc65 has neither,
    // so on cc65 both map to its internal EUNKNOWN.
    API_EDOM,   /* Mathematics argument out of domain of function*/
    API_EILSEQ, /* Invalid or incomplete multibyte or wide character */
} api_errno;

uint8_t api_get_errno_opt(void);
bool api_set_errno_opt(uint8_t opt);

/* Translate to whichever errno numbering the running program selected.
 * Answers 0xFFFF until it has selected one, which is the state api_run leaves
 * at every program start. */
uint16_t api_platform_errno(api_errno num);

/* The longest path any op takes. The boards hold a path in a 256 byte buffer,
 * which is 255 characters and a terminator, so a longer path has nowhere to go
 * on the machine this API was written for. */
#define API_PATH_MAX 255

#define API_OP REGS(0xFFEF)
#define API_ERRNO REGSW(0xFFED)
#define API_STACK REGS(0xFFEC)
#define API_BUSY (REGS(0xFFF2) & 0x80)
#define API_A REGS(0xFFF4)
#define API_X REGS(0xFFF6)
#define API_SREG REGSW(0xFFF8)
#define API_AX (API_A | (API_X << 8))
#define API_AXSREG (API_AX | (API_SREG << 16))

/* The 6502 passes its last argument in API_A, API_AX or API_AXSREG and pushes
 * the others onto the xstack in declaration order, so a handler pops them in
 * reverse and the first parameter comes off last.
 *
 * That first parameter may arrive short -- cc65's time_set pushes a 32 bit
 * long where the call declares a uint64 -- to keep the 6502 code small, so
 * these fill the rest in, with zero for the unsigned forms and with the sign
 * bit for the signed ones. They fail unless the pop empties the xstack.
 */

bool api_pop_uint8_end(uint8_t *data);
bool api_pop_uint16_end(uint16_t *data);
bool api_pop_uint32_end(uint32_t *data);
bool api_pop_uint64_end(uint64_t *data);
bool api_pop_int8_end(int8_t *data);
bool api_pop_int16_end(int16_t *data);
bool api_pop_int32_end(int32_t *data);

static inline bool api_pop_n(void *data, size_t n)
{
    if (XSTACK_SIZE - xstack_ptr < n)
        return false;
    memcpy(data, &xstack[xstack_ptr], n);
    xstack_ptr += n;
    return true;
}

static inline bool api_pop_uint8(uint8_t *data) { return api_pop_n(data, sizeof(uint8_t)); }
static inline bool api_pop_uint16(uint16_t *data) { return api_pop_n(data, sizeof(uint16_t)); }
static inline bool api_pop_uint32(uint32_t *data) { return api_pop_n(data, sizeof(uint32_t)); }
static inline bool api_pop_int8(int8_t *data) { return api_pop_n(data, sizeof(int8_t)); }
static inline bool api_pop_int16(int16_t *data) { return api_pop_n(data, sizeof(int16_t)); }
static inline bool api_pop_int32(int32_t *data) { return api_pop_n(data, sizeof(int32_t)); }

static inline bool api_push_n(const void *data, size_t n)
{
    if (n > xstack_ptr)
        return false;
    xstack_ptr -= n;
    memcpy(&xstack[xstack_ptr], data, n);
    return true;
}

static inline bool api_push_char(const char *data) { return api_push_n(data, sizeof(char)); }
static inline bool api_push_uint8(const uint8_t *data) { return api_push_n(data, sizeof(uint8_t)); }
static inline bool api_push_uint16(const uint16_t *data) { return api_push_n(data, sizeof(uint16_t)); }
static inline bool api_push_uint32(const uint32_t *data) { return api_push_n(data, sizeof(uint32_t)); }
static inline bool api_push_int8(const int8_t *data) { return api_push_n(data, sizeof(int8_t)); }
static inline bool api_push_int16(const int16_t *data) { return api_push_n(data, sizeof(int16_t)); }
static inline bool api_push_int32(const int32_t *data) { return api_push_n(data, sizeof(int32_t)); }

// A call returns by running the seven bytes at $FFF1, which these write. The
// SREG after the RTS is data the caller reads back, not code.
// FFF1 80 FE   BRA -2
// FFF3 A9 FF   LDA #$FF
// FFF5 A2 FF   LDX #$FF
// FFF7 60      RTS
// FFF8 FF FF   .SREG $FF $FF

static inline void api_set_regs_blocked()
{
    // The opcode and its offset are one 16 bit write, so the 6502
    // can never fetch a branch that is half written.
    REGSW(0xFFF1) = 0xFE80;
}

static inline void api_set_regs_released()
{
    // The LDA opcode goes down before $FFF2 clears, because clearing $FFF2
    // makes the branch at $FFF1 a fall through to $FFF3.
    REGS(0xFFF3) = 0xA9;
    REGS(0xFFF2) = 0;
}

static inline void api_set_ax(uint16_t val)
{
    REGSL(0xFFF4) = 0x6000A200 | (val & 0xFF) | ((val << 8) & 0xFF0000);
}

static inline void api_set_axsreg(uint32_t val)
{
    api_set_ax(val);
    API_SREG = val >> 16;
}

/* An API handler must not block. It ends by returning one of these.
 */

// A handler that is still waiting on I/O returns this, and api_task
// dispatches the same op again.
static inline bool api_working(void)
{
    return true;
}

static inline bool api_return(void)
{
    api_set_regs_released();
    API_STACK = xstack[xstack_ptr];
    return false;
}

static inline bool api_return_ax(uint16_t val)
{
    api_set_ax(val);
    return api_return();
}

static inline bool api_return_axsreg(uint32_t val)
{
    api_set_axsreg(val);
    return api_return();
}

static inline bool api_return_errno(api_errno errnum)
{
    API_ERRNO = api_platform_errno(errnum);
    xstack_ptr = XSTACK_SIZE;
    return api_return_axsreg(-1);
}

/* The two bytes are the op the 6502 is parked on and the errno numbering it
 * asked for. The op is enough because a load re-dispatches the same call, and
 * how far that call had got is written down by the handler that owns it. */
#define API_SST_SIZE 2
void api_sst_save(sst_cursor_t *c, unsigned flags);
bool api_sst_load(sst_cursor_t *c, unsigned flags);

#define API_DRIVER DRIVER(nul_init, nul_task, api_task, api_run, api_stop, nul_break, \
    nul_config, nul_config, SST(API_, 1, API_SST_SIZE, api_sst_save, api_sst_load))

#endif /* _CORE_API_API_H_ */
