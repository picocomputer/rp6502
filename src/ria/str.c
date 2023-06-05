/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "str.h"

bool char_is_hex(char ch)
{
    return ((ch >= '0') && (ch <= '9')) ||
           ((ch >= 'A') && (ch <= 'F')) ||
           ((ch >= 'a') && (ch <= 'f'));
}

int char_to_int(char ch)
{
    if ((unsigned int)ch - (unsigned int)'0' < 10u)
        return ch - '0';
    if ((unsigned int)ch - (unsigned int)'A' < 6u)
        return ch - 'A' + 10;
    if ((unsigned int)ch - (unsigned int)'a' < 6u)
        return ch - 'a' + 10;
    return -1;
}

int strnicmp(const char *string1, const char *string2, int n)
{
    while (n--)
    {
        if (!*string1 && !*string2)
            return 0;
        int ch1 = *string1;
        int ch2 = *string2;
        if (ch1 >= 'a' && ch1 <= 'z')
            ch1 -= 32;
        if (ch2 >= 'a' && ch2 <= 'z')
            ch2 -= 32;
        int rc = ch1 - ch2;
        if (rc)
            return rc;
        string1++;
        string2++;
    }
    return 0;
}

bool parse_uint32(const char **args, size_t *len, uint32_t *result)
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
        if (!char_is_hex(ch))
            break;
        uint32_t i = char_to_int(ch);
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

bool parse_rom_name(const char **args, size_t *len, char *name)
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

bool parse_end(const char *args, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        if (args[i] != ' ')
            return false;
    }
    return true;
}
