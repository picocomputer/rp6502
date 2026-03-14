/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "str/str.h"
#include "sys/cpu.h"
#include <fatfs/ff.h>
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

// Shared output buffer for str_parse_string and str_abs_path.
static char str_buf[256];

const char *str_abs_path(const char *path)
{
    char tmp[256];
    size_t drive_len;

    if (strchr(path, ':'))
    {
        if (strlen(path) >= sizeof(tmp))
            return NULL;
        strcpy(tmp, path);
        drive_len = (size_t)(strchr(tmp, ':') - tmp) + 1;
    }
    else
    {
        if (f_getcwd(tmp, sizeof(tmp)) != FR_OK)
            return NULL;
        const char *colon = strchr(tmp, ':');
        if (!colon)
            return NULL;
        drive_len = (size_t)(colon - tmp) + 1;
        if (path[0] == '/')
        {
            if (drive_len + strlen(path) >= sizeof(tmp))
                return NULL;
            strcpy(tmp + drive_len, path);
        }
        else
        {
            size_t cwd_len = strlen(tmp);
            size_t path_len = strlen(path);
            if (cwd_len + 1 + path_len >= sizeof(tmp))
                return NULL;
            if (tmp[cwd_len - 1] != '/')
                tmp[cwd_len++] = '/';
            memcpy(tmp + cwd_len, path, path_len + 1);
        }
    }

    // Uppercase the drive prefix letters (e.g. "msc0:" -> "MSC0:")
    for (size_t i = 0; i + 1 < drive_len; i++)
        tmp[i] = (char)toupper((unsigned char)tmp[i]);

    // Ensure a '/' follows the drive prefix
    if (tmp[drive_len] != '/')
    {
        size_t len = strlen(tmp + drive_len);
        if (strlen(tmp) + 1 >= sizeof(tmp))
            return NULL;
        memmove(tmp + drive_len + 1, tmp + drive_len, len + 1);
        tmp[drive_len] = '/';
    }

    // Normalize: tokenize path segments after "VOL:/", resolve . and ..
    char path_copy[256];
    const char *path_start = tmp + drive_len + 1; // skip leading '/'
    if (strlen(path_start) >= sizeof(path_copy))
        return NULL;
    strcpy(path_copy, path_start);

    const char *segs[128];
    int depth = 0;
    char *seg = path_copy;
    while (*seg)
    {
        char *next = strchr(seg, '/');
        if (next)
            *next = '\0';
        if (*seg != '\0' && strcmp(seg, ".") != 0)
        {
            if (strcmp(seg, "..") == 0)
            {
                if (depth > 0)
                    depth--;
            }
            else
            {
                if (depth >= (int)(sizeof(segs) / sizeof(segs[0])))
                    return NULL;
                segs[depth++] = seg;
            }
        }
        seg = next ? next + 1 : seg + strlen(seg);
    }

    if (drive_len > 255)
        return NULL;
    memcpy(str_buf, tmp, drive_len);
    size_t out = drive_len;
    if (depth == 0)
    {
        if (out + 1 > 255)
            return NULL;
        str_buf[out++] = '/';
    }
    else
    {
        for (int i = 0; i < depth; i++)
        {
            size_t slen = strlen(segs[i]);
            if (out + 1 + slen > 255)
                return NULL;
            str_buf[out++] = '/';
            if (drive_len == 1) // ":" installed ROM
                for (size_t k = 0; k < slen; k++)
                    str_buf[out++] = (char)toupper((unsigned char)segs[i][k]);
            else
            {
                memcpy(str_buf + out, segs[i], slen);
                out += slen;
            }
        }
    }
    str_buf[out] = '\0';
    return str_buf;
}

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

const char *str_parse_string(const char **args)
{
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
                case 'n':
                    str_buf[out++] = '\n';
                    break;
                case 't':
                    str_buf[out++] = '\t';
                    break;
                case 'r':
                    str_buf[out++] = '\r';
                    break;
                case 'a':
                    str_buf[out++] = '\a';
                    break;
                case 'b':
                    str_buf[out++] = '\b';
                    break;
                case 'f':
                    str_buf[out++] = '\f';
                    break;
                case 'v':
                    str_buf[out++] = '\v';
                    break;
                case 'x':
                {
                    if (!isxdigit((unsigned char)(*args)[j + 1]))
                        return NULL;
                    uint32_t val = 0;
                    while (isxdigit((unsigned char)(*args)[j + 1]))
                    {
                        val = val * 16 + (uint32_t)str_xdigit_to_int((*args)[++j]);
                    }
                    if ((val & 0xFF) == 0)
                        return NULL;
                    str_buf[out++] = (char)(val & 0xFF);
                    break;
                }
                case '0':
                case '1':
                case '2':
                case '3':
                case '4':
                case '5':
                case '6':
                case '7':
                {
                    uint32_t val = (uint32_t)((*args)[j] - '0');
                    if ((*args)[j + 1] >= '0' && (*args)[j + 1] <= '7')
                        val = val * 8 + (uint32_t)((*args)[++j] - '0');
                    if ((*args)[j + 1] >= '0' && (*args)[j + 1] <= '7')
                        val = val * 8 + (uint32_t)((*args)[++j] - '0');
                    if ((val & 0xFF) == 0)
                        return NULL;
                    str_buf[out++] = (char)(val & 0xFF);
                    break;
                }
                default:
                    str_buf[out++] = (*args)[j];
                    break;
                }
            }
            else
                str_buf[out++] = (*args)[j];
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
            str_buf[out++] = (*args)[j++];
        }
        while ((*args)[j] == ' ')
            j++;
        *args += j;
    }
    str_buf[out] = 0;
    return str_buf;
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
