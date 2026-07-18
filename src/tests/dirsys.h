/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Test helpers to drive the host filesystem syscall handlers (emu/host/msc.c
 * msc_api_*) the way the 6502 does: stage the args on the xstack / in the API
 * registers, call the handler, then read the AX result and decode any pushed
 * FILINFO / string. The handlers are the unit under test; they call the platform
 * primitives (emu/plat.h) for the actual OS operations.
 */

#ifndef _EMU_TESTS_DIRSYS_H_
#define _EMU_TESTS_DIRSYS_H_

#include "api/api.h"
#include "emu/sys/mem.h" /* xstack */
#include "fatfs/ff.h"    /* FILINFO */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Put a NUL-terminated path where a dir handler reads it: &xstack[xstack_ptr]. */
static inline void dsys_path(const char *p)
{
    size_t n = strlen(p) + 1;
    xstack_ptr = (uint16_t)(XSTACK_SIZE - n);
    memcpy(&xstack[xstack_ptr], p, n);
}

/* Stage a descriptor in A on an empty xstack: for the ops that take a dir handle
 * and no path (readdir/telldir/closedir/rewinddir). The empty stack mirrors what
 * the 6502 leaves after consuming the previous op's pushed result. */
static inline void dsys_des(int des)
{
    API_A = (uint8_t)des;
    xstack_ptr = XSTACK_SIZE;
}

/* chmod args: mask in A, then [attr][path] on the xstack. */
static inline void dsys_chmod(uint8_t mask, uint8_t attr, const char *path)
{
    API_A = mask;
    size_t n = strlen(path) + 1;
    xstack_ptr = (uint16_t)(XSTACK_SIZE - 1 - n);
    xstack[xstack_ptr] = attr;
    memcpy(&xstack[xstack_ptr + 1], path, n);
}

/* utime args: [crdate][ftime][fdate][path] on the xstack (crtime in AX is ignored
 * by the host backend, so we leave it zero). */
static inline void dsys_utime(uint16_t ftime, uint16_t fdate, const char *path)
{
    uint16_t crdate = 0;
    size_t n = strlen(path) + 1;
    xstack_ptr = (uint16_t)(XSTACK_SIZE - 6 - n);
    memcpy(&xstack[xstack_ptr], &crdate, 2);
    memcpy(&xstack[xstack_ptr + 2], &ftime, 2);
    memcpy(&xstack[xstack_ptr + 4], &fdate, 2);
    memcpy(&xstack[xstack_ptr + 6], path, n);
}

/* The 16-bit AX a handler returned: 0 (or a descriptor / length) on success,
 * -1 on error (the api_errno option defaults to NULL, so errors read back -1). */
static inline int16_t dsys_ax(void)
{
    return (int16_t)(uint16_t)(API_A | (API_X << 8));
}

/* The 32-bit A:X:SREG a handler returned (telldir). */
static inline int32_t dsys_axsreg(void)
{
    uint16_t lo = (uint16_t)(API_A | (API_X << 8));
    return (int32_t)((uint32_t)lo | ((uint32_t)API_SREG << 16));
}

/* Decode the FILINFO a stat/readdir handler pushed (reverse of dir_push_filinfo). */
static inline void dsys_filinfo(FILINFO *fno)
{
    size_t p = xstack_ptr;
    memcpy(&fno->fsize, &xstack[p], 4), p += 4;
    memcpy(&fno->fdate, &xstack[p], 2), p += 2;
    memcpy(&fno->ftime, &xstack[p], 2), p += 2;
    memcpy(&fno->crdate, &xstack[p], 2), p += 2;
    memcpy(&fno->crtime, &xstack[p], 2), p += 2;
    fno->fattrib = xstack[p], p += 1;
    memcpy(fno->altname, &xstack[p], 13), p += 13;
    memcpy(fno->fname, &xstack[p], 256);
}

/* Read a string a handler relocated to the top of the xstack (getcwd). */
static inline void dsys_str(char *out, size_t sz)
{
    snprintf(out, sz, "%s", (const char *)&xstack[xstack_ptr]);
}

/* Read the two 32-bit words a getfree handler pushed (free then total). */
static inline void dsys_getfree(uint32_t *fre, uint32_t *tot)
{
    memcpy(fre, &xstack[xstack_ptr], 4);
    memcpy(tot, &xstack[xstack_ptr + 4], 4);
}

#endif /* _EMU_TESTS_DIRSYS_H_ */
