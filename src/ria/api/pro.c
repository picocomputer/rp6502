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
// to zero terminated strings within pro_argv.
// Maintans no space between pointers and chars.
static uint8_t pro_argv[XSTACK_SIZE];

uint16_t pro_argv_count(void)
{
    for (uint16_t i = 0; i < XSTACK_SIZE / 2; i++)
        if (pro_argv[i * 2] == 0 && pro_argv[i * 2 + 1] == 0)
            return i;
    return 0;
}

void pro_argv_clear(void)
{
    pro_argv[0] = pro_argv[1] = 0;
}

static uint16_t pro_argv_size(void)
{
    uint16_t count = pro_argv_count();
    uint16_t pos = (count + 1) * 2;
    for (uint16_t i = 0; i < count; i++)
        pos += strlen((const char *)&pro_argv[pos]) + 1;
    return pos;
}

static bool pro_argv_validate(void)
{
    uint16_t count = pro_argv_count();
    if ((count + 1) * 2 > XSTACK_SIZE)
        return false;
    for (uint16_t i = 0; i < count; i++)
    {
        uint16_t offset = pro_argv[i * 2] | ((uint16_t)pro_argv[i * 2 + 1] << 8);
        if (offset >= XSTACK_SIZE)
            return false;
    }
    return pro_argv_size() <= XSTACK_SIZE;
}

bool pro_argv_append(const char *str)
{
    uint16_t count = pro_argv_count();
    uint16_t old_strings_start = (count + 1) * 2;
    uint16_t old_size = pro_argv_size();
    uint16_t strings_len = old_size - old_strings_start;
    uint16_t new_str_len = (uint16_t)strlen(str) + 1;
    if (old_size + 2 + new_str_len > XSTACK_SIZE)
        return false;
    memmove(&pro_argv[old_strings_start + 2], &pro_argv[old_strings_start], strings_len);
    for (uint16_t i = 0; i < count; i++)
    {
        uint16_t offset = pro_argv[i * 2] | ((uint16_t)pro_argv[i * 2 + 1] << 8);
        offset += 2;
        pro_argv[i * 2] = offset & 0xFF;
        pro_argv[i * 2 + 1] = offset >> 8;
    }
    uint16_t new_offset = old_strings_start + 2 + strings_len;
    pro_argv[count * 2] = new_offset & 0xFF;
    pro_argv[count * 2 + 1] = new_offset >> 8;
    pro_argv[(count + 1) * 2] = 0;
    pro_argv[(count + 1) * 2 + 1] = 0;
    memcpy(&pro_argv[new_offset], str, new_str_len);
    return true;
}

const char *pro_argv_index(uint16_t idx)
{
    if (idx >= pro_argv_count())
        return NULL;
    uint16_t offset = pro_argv[idx * 2] | ((uint16_t)pro_argv[idx * 2 + 1] << 8);
    return (const char *)&pro_argv[offset];
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
