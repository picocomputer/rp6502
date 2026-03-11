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

// TODO, saved
// The monitor (mon.c) sometimes uses str_deprecated which
// was put tgogether in a hurry. It's time to do it right.
// We need a str_something helper which will parse a quoted-escaped
// string in the usual manner. Max output len 255, as usual for our strings.
// It should provide its own storage which is only valid until the next call.
// All non-numeric strings, including the commands like "load, help, set, etc."
// should parse through str_something.
// the str_parse_rom_name in set.c should be changed to str_something.
// the logic of str_parse_rom_name itself should be moved to rom.c as validation.
// rom_is_installed should use that validation before opening lfs which may
// allow us to remove some extra checks.
// The load and "{rom}" commands should use pro_argv_clear and pro_argv_append
// to populate the exectuable name and arguments, which we don't yet support.
// don't worry about what happpens to argv later, just do the parsing right now.
// The entire call chain string from static mon_enter needs to be checked for
// internally coded checks for strings by searching for spaces.
// Make sure our space-collapsing between arguments is maintained.

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


// int get_argv(char *const argv[]);
bool pro_api_argv(void)
{
    uint16_t size = pro_argv_size();
    xstack_ptr = XSTACK_SIZE - size;
    memcpy(&xstack[xstack_ptr], pro_argv, size);
    return api_return_ax(size);
}

// int exec(char *const argv[]);
bool pro_api_exec(void)
{
    uint16_t size = (uint16_t)(XSTACK_SIZE - xstack_ptr);
    memcpy(pro_argv, &xstack[xstack_ptr], size);
    memset(&pro_argv[size], 0, XSTACK_SIZE - size);
    xstack_ptr = XSTACK_SIZE;
    if (!pro_argv_validate())
    {
        pro_argv_clear();
        return api_return_errno(API_EINVAL);
    }
    return api_return_ax(0);
}
