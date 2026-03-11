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
#define DBG(...) printf(__VA_ARGS__)
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

bool str_parse_uint8(const char **args, uint8_t *result)
{
    uint32_t result32;
    if (str_parse_uint32(args, &result32) && result32 < 0x100)
    {
        *result = result32;
        return true;
    }
    return false;
}

bool str_parse_uint16(const char **args, uint16_t *result)
{
    uint32_t result32;
    if (str_parse_uint32(args, &result32) && result32 < 0x10000)
    {
        *result = result32;
        return true;
    }
    return false;
}

bool str_parse_uint32(const char **args, uint32_t *result)
{
    size_t i = 0;
    while ((*args)[i] == ' ')
        i++;
    size_t start = i;
    uint32_t base = 10;
    uint32_t value = 0;
    uint32_t prefix = 0;
    if ((*args)[i] == '$')
    {
        base = 16;
        prefix = 1;
    }
    else if ((*args)[i] == '0' &&
             ((*args)[i + 1] == 'x' || (*args)[i + 1] == 'X'))
    {
        base = 16;
        prefix = 2;
    }
    i = start + prefix;
    if (!(*args)[i])
        return false;
    for (; (*args)[i]; i++)
    {
        char ch = (*args)[i];
        if (base == 10 && !isdigit(ch))
            break;
        if (base == 16 && !isxdigit(ch))
            break;
        uint32_t digit = str_xdigit_to_int(ch);
        if (digit >= base)
            return false;
        if (value > (UINT32_MAX - digit) / base)
            return false;
        value = value * base + digit;
    }
    if (i == start + prefix)
        return false;
    if ((*args)[i] && (*args)[i] != ' ')
        return false;
    while ((*args)[i] == ' ')
        i++;
    *args += i;
    *result = value;
    return true;
}

char *str_parse_string(const char **args)
{
    static char buf[256];
    size_t i = 0;
    while ((*args)[i] == ' ')
        i++;
    if (!(*args)[i])
        return NULL;
    *args += i;
    size_t out = 0;
    if ((*args)[0] == '"')
    {
        size_t j = 1;
        while ((*args)[j] && (*args)[j] != '"')
        {
            if (out >= 255)
                return NULL;
            if ((*args)[j] == '\\' && (*args)[j + 1])
            {
                j++;
                switch ((*args)[j])
                {
                case 'n': buf[out++] = '\n'; break;
                case 't': buf[out++] = '\t'; break;
                case 'r': buf[out++] = '\r'; break;
                default:  buf[out++] = (*args)[j]; break;
                }
            }
            else
                buf[out++] = (*args)[j];
            j++;
        }
        if (!(*args)[j])
            return NULL; // unclosed quote
        j++;             // skip closing "
        if ((*args)[j] && (*args)[j] != ' ')
            return NULL; // garbage after closing quote
        while ((*args)[j] == ' ')
            j++;
        *args += j;
    }
    else
    {
        size_t j = 0;
        while ((*args)[j] && (*args)[j] != ' ')
        {
            if (out >= 255)
                return NULL;
            buf[out++] = (*args)[j++];
        }
        while ((*args)[j] == ' ')
            j++;
        *args += j;
    }
    buf[out] = 0;
    return buf;
}

bool str_parse_end(const char *args)
{
    while (*args)
    {
        if (*args != ' ')
            return false;
        args++;
    }
    return true;
}
