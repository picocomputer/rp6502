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
#include <sys/cdefs.h>

#if defined(DEBUG_RIA_STR) || defined(DEBUG_RIA_STR_STR)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

static_assert(CPU_PHI2_MIN_KHZ >= 0); // catch missing include
#define STR_PHI2_MIN_MAX __XSTRING(CPU_PHI2_MIN_KHZ) "-" __XSTRING(CPU_PHI2_MAX_KHZ)
#define STR_RP6502_CODE_PAGE __XSTRING(RP6502_CODE_PAGE)

// Put string literals into flash.
#define X(name, value) \
    const char __in_flash(__XSTRING(name)) name[] = value;
#include "str.inc"
#include RP6502_LOCALE
#undef X

// Shared output buffer for str_parse_string and str_abs_path.
static char str_buf[256];

const char *str_abs_path(const char *path)
{
    size_t drive_len;
    const char *segs_src;

    if (strchr(path, ':'))
    {
        const char *colon = strchr(path, ':');
        drive_len = (size_t)(colon - path) + 1;
        if (drive_len >= sizeof(str_buf))
            return NULL;
        for (size_t i = 0; i + 1 < drive_len; i++)
            str_buf[i] = (char)toupper((unsigned char)path[i]);
        str_buf[drive_len - 1] = ':';
        segs_src = colon + 1;
        if (*segs_src == '/')
            segs_src++;
    }
    else
    {
        if (f_getcwd(str_buf, sizeof(str_buf)) != FR_OK)
            return NULL;
        const char *colon = strchr(str_buf, ':');
        if (!colon)
            return NULL;
        drive_len = (size_t)(colon - str_buf) + 1;
        segs_src = path;
        if (*segs_src == '/')
            segs_src++;
    }

    // For relative paths start at the end of the CWD already in str_buf.
    // For absolute paths start fresh after the drive prefix.
    size_t out;
    if (!strchr(path, ':') && path[0] != '/')
    {
        out = strlen(str_buf);
        while (out > drive_len && str_buf[out - 1] == '/')
            out--;
    }
    else
    {
        out = drive_len;
    }

    // Write segments into str_buf, resolve . and ..
    const char *seg = segs_src;
    while (*seg)
    {
        const char *next = strchr(seg, '/');
        size_t slen = next ? (size_t)(next - seg) : strlen(seg);
        if (slen == 0 || (slen == 1 && seg[0] == '.'))
        {
            // skip empty segment or "."
        }
        else if (slen == 2 && seg[0] == '.' && seg[1] == '.')
        {
            if (out > drive_len)
            {
                out--;
                while (out > drive_len && str_buf[out] != '/')
                    out--;
            }
        }
        else
        {
            if (out + 1 + slen > 255)
                return NULL;
            str_buf[out++] = '/';
            if (drive_len == 1) // ":" installed ROM
                for (size_t k = 0; k < slen; k++)
                    str_buf[out++] = (char)toupper((unsigned char)seg[k]);
            else
            {
                memcpy(str_buf + out, seg, slen);
                out += slen;
            }
        }
        seg = next ? next + 1 : seg + slen;
    }

    if (out == drive_len)
        str_buf[out++] = '/';
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
    size_t j = 0;
    while ((*args)[j] && (*args)[j] != ' ')
    {
        if ((*args)[j] == '"')
        {
            j++; // skip opening "
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
        }
        else
        {
            if (out >= 255)
                return NULL;
            str_buf[out++] = (*args)[j++];
        }
    }
    while ((*args)[j] == ' ')
        j++;
    *args += j;
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
