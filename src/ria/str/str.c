/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "str/str.h"
#include "sys/cpu.h"
#include <string.h>
#include <ctype.h>
#include <pico.h>

#if defined(DEBUG_RIA_STR) || defined(DEBUG_RIA_STR_STR)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// Stringify various defines for inclusion in string literals.
// This scope hides it from accidental use as a RAM literal.
#define _STRINGIFY(x) #x
#define STRINGIFY(x) _STRINGIFY(x)
static_assert(CPU_PHI2_MIN_KHZ >= 0); // catch missing include
#define STR_PHI2_MIN_MAX STRINGIFY(CPU_PHI2_MIN_KHZ) "-" STRINGIFY(CPU_PHI2_MAX_KHZ)
#define STR_RP6502_CODE_PAGE STRINGIFY(RP6502_CODE_PAGE)

// Part 2 of putting string literals into flash.
#define X(name, value) \
    const char __in_flash(STRINGIFY(name)) name[] = value;
#include "str.inc"
#include RP6502_LOCALE
#undef X

int str_xdigit_to_int(char ch)
{
    if (ch >= '0' && ch <= '9')
        ch -= '0';
    else if (ch >= 'A' && ch <= 'F')
        ch -= 'A' - 10;
    else if (ch >= 'a' && ch <= 'f')
        ch -= 'a' - 10;
    return ch;
}

bool str_parse_string(const char **args, size_t *len, char *dest, size_t maxlen)
{
    size_t cpylen = *len;
    while (cpylen && (*args)[cpylen - 1] == ' ')
        cpylen--;
    if (cpylen < maxlen)
    {
        memcpy(dest, *args, cpylen);
        dest[cpylen] = 0;
        *len -= cpylen;
        *args += cpylen;
        return true;
    }
    dest[0] = 0;
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
        if (base == 10 && !isdigit(ch))
            break;
        if (base == 16 && !isxdigit(ch))
            break;
        uint32_t digit = str_xdigit_to_int(ch);
        if (digit >= base)
            return false;
        value = value * base + digit;
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
    size_t i;
    for (i = 0; i < *len; i++)
    {
        if ((*args)[i] != ' ')
            break;
    }
    if (i == *len)
        return false;
    size_t name_len;
    for (name_len = 0; i < *len && name_len < LFS_NAME_MAX; i++)
    {
        char ch = toupper((*args)[i]);
        if (ch == ' ')
            break;
        if (isupper(ch) || (name_len && isdigit(ch)))
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
