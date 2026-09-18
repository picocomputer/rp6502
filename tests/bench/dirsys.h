/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _EMU_TESTS_DIRSYS_H_
#define _EMU_TESTS_DIRSYS_H_

#include "core/api/api.h"
#include "core/ria/regs.h"
#include "core/api/dir.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static inline void dsys_path(const char *p)
{
    size_t n = strlen(p) + 1;
    xstack_ptr = (uint16_t)(XSTACK_SIZE - n);
    memcpy(&xstack[xstack_ptr], p, n);
}

static inline void dsys_des(int des)
{
    API_A = (uint8_t)des;
    xstack_ptr = XSTACK_SIZE;
}

static inline void dsys_chmod(uint8_t mask, uint8_t attr, const char *path)
{
    API_A = mask;
    size_t n = strlen(path) + 1;
    xstack_ptr = (uint16_t)(XSTACK_SIZE - 1 - n);
    xstack[xstack_ptr] = attr;
    memcpy(&xstack[xstack_ptr + 1], path, n);
}

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

static inline int16_t dsys_ax(void)
{
    return (int16_t)(uint16_t)(API_A | (API_X << 8));
}

static inline int32_t dsys_axsreg(void)
{
    uint16_t lo = (uint16_t)(API_A | (API_X << 8));
    return (int32_t)((uint32_t)lo | ((uint32_t)API_SREG << 16));
}

static inline void dsys_filinfo(f_stat_t *fno)
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

static inline void dsys_str(char *out, size_t sz)
{
    snprintf(out, sz, "%s", (const char *)&xstack[xstack_ptr]);
}

static inline void dsys_getfree(uint32_t *fre, uint32_t *tot)
{
    memcpy(fre, &xstack[xstack_ptr], 4);
    memcpy(tot, &xstack[xstack_ptr + 4], 4);
}

#endif /* _EMU_TESTS_DIRSYS_H_ */
