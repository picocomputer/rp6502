/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_ROM_WINDOW_H_
#define _CORE_ROM_WINDOW_H_

/* A read-only window onto a stretch of a .rp6502: the asset a program opened
 * by name, which is not a file the machine has but a range inside one.
 *
 * The bookkeeping is the same wherever the bytes live -- how much is left,
 * what a seek past the end means, that reading at the end is not an error --
 * so a machine supplies only the one thing it alone can do, which is fetch
 * bytes at an offset.
 */

#include "core/api/api.h"
#include "core/api/std.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    bool used;
    uint32_t base, len, pos;
    int fd; /* the machine's own handle for this window, or -1 */
} rom_window_t;

/* Up to count bytes at an absolute offset. Absolute rather than "read on from
 * where you were", because a machine may be sharing one handle between every
 * open window and the loader. STD_PENDING is allowed. */
typedef std_rw_result (*rom_window_fetch_fn)(rom_window_t *w, uint32_t at, char *buf,
                                          uint32_t count, uint32_t *got,
                                          api_errno *err);

typedef struct
{
    rom_window_t *slots;
    uint8_t max;
    rom_window_fetch_fn fetch;
} rom_window_pool_t;

/* Take a slot for [base, base+len) with the machine's handle, or -1 + *err. */
int rom_window_alloc(const rom_window_pool_t *p, uint32_t base, uint32_t len, int fd,
                  api_errno *err);

/* The window a descriptor names, or NULL when it names none. */
rom_window_t *rom_window_get(const rom_window_pool_t *p, int desc);

std_rw_result rom_window_read(const rom_window_pool_t *p, int desc, char *buf,
                           uint32_t count, uint32_t *got, api_errno *err);

int rom_window_lseek(const rom_window_pool_t *p, int desc, int8_t whence, int32_t off,
                  int32_t *pos, api_errno *err);

#endif /* _CORE_ROM_WINDOW_H_ */
