/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/api.h"
#include "api/pro.h"
#include <stdio.h>

#if defined(DEBUG_RIA_API) || defined(DEBUG_RIA_API_PRO)
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// A zero terminated list of uint16 which points
// to zero terminated strings within pro_argc.
// Maintans no space between pointers and chars.
static uint8_t pro_argv[XSTACK_SIZE];

// TODO
// implement pro_argv_*
// expose  non statics in pro.h
// do not cache anything, performance isn't important

static bool pro_argv_validate(void)
{
    // Valid when all other pro_argv_* never goes out of bounds
}

static uint16_t pro_argv_size(void)
{
}

uint16_t pro_argv_count(void)
{
}

void pro_argv_clear(void)
{
    pro_argv[0] = pro_argv[1] = 0;
}

bool pro_argv_append(const char *str)
{
}

const char *pro_argv_index(uint16_t idx)
{
}


// int get_argv(char *const argv[], int size);
bool pro_api_argv(void)
{
    return api_return_errno(API_ENOSYS);
}
// int execv(const char *path, char *const argv[]);
bool pro_api_execv(void)
{
    return api_return_errno(API_ENOSYS);
}
