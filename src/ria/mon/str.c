/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mon/str.h"
#include <string.h>

#if defined(DEBUG_RIA_MON) || defined(DEBUG_RIA_MON_STR)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

bool str_char_is_hex(char ch)
{
    return ((ch >= '0') && (ch <= '9')) ||
           ((ch >= 'A') && (ch <= 'F')) ||
           ((ch >= 'a') && (ch <= 'f'));
}

int str_char_to_int(char ch)
{
    if ((unsigned int)ch - (unsigned int)'0' < 10u)
        return ch - '0';
    if ((unsigned int)ch - (unsigned int)'A' < 6u)
        return ch - 'A' + 10;
    if ((unsigned int)ch - (unsigned int)'a' < 6u)
        return ch - 'a' + 10;
    return -1;
}

bool str_parse_string(const char **args, size_t *len, char *dest, size_t size)
{
    size_t cpylen = *len;
    while (cpylen && (*args)[cpylen - 1] == ' ')
        cpylen--;
    if (cpylen < size)
    {
        memcpy(dest, *args, cpylen);
        dest[cpylen] = 0;
        *len -= cpylen;
        *args += cpylen;
        return true;
    }
    return false;
}

bool str_parse_uint8(const char **args, size_t *len, uint8_t *result)
{
    uint32_t result32;
    if (str_parse_uint32(args, len, &result32) && result32 < 0x100)
    {
        *result = result32;
        return true;
    }
    return false;
}

bool str_parse_uint16(const char **args, size_t *len, uint16_t *result)
{
    uint32_t result32;
    if (str_parse_uint32(args, len, &result32) && result32 < 0x10000)
    {
        *result = result32;
        return true;
    }
    return false;
}

bool str_parse_uint32(const char **args, size_t *len, uint32_t *result)
{
    size_t i;
    for (i = 0; i < *len; i++)
    {
        if ((*args)[i] != ' ')
            break;
    }
    uint32_t base = 10;
    uint32_t value = 0;
    uint32_t prefix = 0;
    if (i < (*len) && (*args)[i] == '$')
    {
        base = 16;
        prefix = 1;
    }
    else if (i + 1 < *len && (*args)[i] == '0' &&
             ((*args)[i + 1] == 'x' || (*args)[i + 1] == 'X'))
    {
        base = 16;
        prefix = 2;
    }
    i = prefix;
    if (i == *len)
        return false;
    for (; i < *len; i++)
    {
        char ch = (*args)[i];
        if (base == 10 && (ch < '0' || ch > '9'))
            break;
        if (base == 16 && !str_char_is_hex(ch))
            break;
        uint32_t i = str_char_to_int(ch);
        if (i >= base)
            return false;
        value = value * base + i;
    }
    if (i == prefix)
        return false;
    if (i < *len && (*args)[i] != ' ')
        return false;
    for (; i < *len; i++)
        if ((*args)[i] != ' ')
            break;
    *len -= i;
    *args += i;
    *result = value;
    return true;
}

bool str_parse_rom_name(const char **args, size_t *len, char *name)
{
    name[0] = 0;
    size_t name_len = 0;
    size_t i;
    for (i = 0; i < *len; i++)
    {
        if ((*args)[i] != ' ')
            break;
    }
    if (i == *len)
        return false;
    for (; i < *len && name_len < LFS_NAME_MAX; i++)
    {
        char ch = (*args)[i];
        if (ch == ' ')
            break;
        if (ch >= 'a' && ch <= 'z')
            ch -= 32;
        if ((ch >= 'A' && ch <= 'Z') ||
            (name_len && ch >= '0' && ch <= '9'))
        {
            name[name_len++] = ch;
            continue;
        }
        name[0] = 0;
        return false;
    }
    if (!name_len)
        return false;
    if (i < *len && (*args)[i] != ' ')
    {
        name[0] = 0;
        return false;
    }
    for (; i < *len; i++)
        if ((*args)[i] != ' ')
            break;
    *len -= i;
    *args += i;
    name[name_len] = 0;
    return true;
}

bool str_parse_end(const char *args, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        if (args[i] != ' ')
            return false;
    }
    return true;
}
