/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/api/oem.h"
#include "core/str/str.h"
#include "core/cfg.h"
#include "core/cpu.h"
#include <fatfs/ff.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdio.h>
#include "host.h"
#include <assert.h>

#if defined(DEBUG_RIA_STR) || defined(DEBUG_RIA_STR_STR)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

/* Two-level so an argument that is itself a macro expands first, which is
 * the whole reason these are here: glibc's __CONCAT expands once. */
#define STR_XSTR1(x) #x
#define STR_XSTR(x) STR_XSTR1(x)
#define STR_CAT1(a, b) a##b
#define STR_CAT(a, b) STR_CAT1(a, b)

static_assert(CPU_PHI2_MIN_KHZ >= 0); // catch missing include
#define STR_PHI2_MIN_MAX STR_XSTR(CPU_PHI2_MIN_KHZ) "-" STR_XSTR(CPU_PHI2_MAX_KHZ)

// Non-localized string literals: flash, or RAM with XR().
#define X(name, value) \
    const char HOST_IN_FLASH(STR_XSTR(name)) name[] = value;
#define XR(name, value) \
    const char HOST_NOT_IN_FLASH(STR_XSTR(name)) name[] = value;
#include "core/def/str_sys.def"
#undef X
#undef XR

// Per-locale string storage and tables, generated from def/str.def. Adding a
// locale touches only def/. STR_ID pairs the locale's XSUFFIX with a string
// id to form one name shared by the storage pass and the table pass.
#define STR_ID_(loc, name) str_loc_##loc##_##name
#define STR_ID(loc, name) STR_ID_(loc, name)

// Each localized string is its own external flash-placed array. External
// linkage is required: a static array or a bare literal initializer gets
// merged by LTO into .rodata.str, which a copy_to_ram build places in RAM.
#define XBEGIN(code, verbose, cp)
#define XEND()
#define X(name, value) const char HOST_IN_FLASH("str_loc") STR_ID(XSUFFIX, name)[] = value;
#include "core/def/str.def"
#undef XBEGIN
#undef XEND
#undef X

// Each XBEGIN opens one flash-placed table of pointers sized to str_loc_id; the
// [name] designators place each string by its id, so line order within a
// locale file is irrelevant.
#define XBEGIN(code, verbose, cp) \
    static const char *const HOST_IN_FLASH("str_tab") STR_CAT(str_tab_, XSUFFIX)[STR_LOC_COUNT] = {
#define XEND() \
    }          \
    ;
#define X(name, value) [name] = STR_ID(XSUFFIX, name),
#include "core/def/str.def"
#undef XBEGIN
#undef XEND
#undef X
#undef STR_ID
#undef STR_ID_

#define XBEGIN(code, verbose, cp) STR_CAT(str_tab_, XSUFFIX),
#define XEND()
#define X(name, value)
static const char *const *const HOST_IN_FLASH("str_tabs") str_tabs[] = {
#include "core/def/str.def"
};
#undef XBEGIN
#undef XEND
#undef X

// Parallel registry arrays, ordered by def/str.def.
#define XBEGIN(code, verbose, cp) code,
#define XEND()
#define X(name, value)
static const char *const HOST_IN_FLASH("str_locale_names") str_locale_names[] = {
#include "core/def/str.def"
};
#undef XBEGIN
#undef XEND
#undef X

#define XBEGIN(code, verbose, cp) verbose,
#define XEND()
#define X(name, value)
static const char *const HOST_IN_FLASH("str_locale_verbose") str_locale_verbose[] = {
#include "core/def/str.def"
};
#undef XBEGIN
#undef XEND
#undef X

#define XBEGIN(code, verbose, cp) cp,
#define XEND()
#define X(name, value)
static const uint16_t HOST_IN_FLASH("str_locale_cp") str_locale_cp[] = {
#include "core/def/str.def"
};
#undef XBEGIN
#undef XEND
#undef X

// Order no longer matters (entries are placed by id), but every locale must
// still define each string exactly once. Count each locale's entries and
// assert the total; a missing or extra line trips here, a duplicate id trips
// -Werror=override-init in the table pass above.
#define XBEGIN(code, verbose, cp) enum \
{                                      \
    STR_CAT(str_count_, XSUFFIX) = 0
#define XEND() \
    }          \
    ;
#define X(name, value) +1
#include "core/def/str.def"
#undef XBEGIN
#undef XEND
#undef X
#define XBEGIN(code, verbose, cp) \
    static_assert((int)STR_CAT(str_count_, XSUFFIX) == STR_LOC_COUNT, "locale " code " string count mismatch");
#define XEND()
#define X(name, value)
#include "core/def/str.def"
#undef XBEGIN
#undef XEND
#undef X

static int str_locale_index;
static bool str_locale_loaded;

const char *S(int id)
{
    return str_tabs[str_locale_index][id];
}

// Switch the active string table (clamped). Internal; the locale is selected
// by name through str_set_locale / str_load_locale.
static void str_select_locale(int index)
{
    int count = (int)(sizeof str_tabs / sizeof str_tabs[0]);
    str_locale_index = (index >= 0 && index < count) ? index : 0;
}

// Find a locale by short name. Falls back to the build default
// (RP6502_LOCALE) when name is empty or unknown, mirroring keyboard.
static int str_sanitize_locale(const char *name)
{
    const int count = sizeof(str_locale_names) / sizeof(str_locale_names)[0];
    int default_index = 0;
    int found_index = -1;
    for (int i = 0; i < count; i++)
    {
        if (!strcasecmp(str_locale_names[i], STR_XSTR(RP6502_LOCALE)))
            default_index = i;
        if (!strcasecmp(str_locale_names[i], name))
            found_index = i;
    }
    return found_index < 0 ? default_index : found_index;
}

// Switch the string table and push the locale's default code page to oem
// (oem only acts on it in auto mode).
static void str_apply_locale(int index)
{
    str_select_locale(index);
    oem_locale_changed(str_locale_cp[index]);
}

void HOST_IN_FLASH("str_init") str_init(void)
{
    if (!str_locale_loaded)
        str_apply_locale(str_sanitize_locale(""));
}

int str_locales_response(char *buf, size_t buf_size, int state, unsigned width)
{
    (void)width;
    const int count = sizeof(str_locale_names) / sizeof(str_locale_names)[0];
    if (state < 0 || state >= count)
        return -1;
    int maxlen = 0;
    for (int i = 0; i < count; i++)
    {
        int thislen = strlen(str_locale_names[i]);
        if (thislen > maxlen)
            maxlen = thislen;
    }
    snprintf(buf, buf_size,
             "  %*s - \a%s\n",
             maxlen, str_locale_names[state],
             str_locale_verbose[state]);
    return state + 1;
}

void str_load_locale(const char *name)
{
    str_apply_locale(str_sanitize_locale(name));
    str_locale_loaded = true;
}

bool str_set_locale(const char *name)
{
    int new_index = str_sanitize_locale(name);
    if (strcasecmp(name, str_locale_names[new_index]))
        return false;
    if (str_locale_index != new_index)
        str_apply_locale(new_index);
    cfg_save();
    return true;
}

const char *str_get_locale(void)
{
    return str_locale_names[str_locale_index];
}

const char *str_get_locale_verbose(void)
{
    return str_locale_verbose[str_locale_index];
}

// Shared output buffer for str_parse_string.
static char str_buf[256];

// Case-insensitive equality of two OEM strings in the active code page,
// matching FatFs's name lookup: convert each OEM byte to Unicode then
// upper-case via ff_wtoupper. strcasecmp would only fold ASCII.
bool str_oem_eq(const char *a, const char *b)
{
    WORD cp = oem_get_code_page_run();
    for (;;)
    {
        WCHAR ua = ff_oem2uni((unsigned char)*a, cp);
        WCHAR ub = ff_oem2uni((unsigned char)*b, cp);
        if (ff_wtoupper(ua) != ff_wtoupper(ub))
            return false;
        if (!*a)
            return true;
        a++;
        b++;
    }
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

void str_size(uint64_t bytes, char *out, size_t out_size)
{
    const char *unit;
    double size;
    if (bytes < 5000000ULL)
    {
        // Floppy-era media: KB, rolling to MB, trailing zeros stripped.
        unit = "KB";
        size = bytes / 1024.0;
        if (size >= 1000)
        {
            unit = "MB";
            size /= 1000;
        }
        char num[16];
        snprintf(num, sizeof(num), "%.3f", size);
        char *p = num + strlen(num) - 1;
        while (*p == '0')
            *p-- = '\0';
        if (*p == '.')
            *p = '\0';
        snprintf(out, out_size, "%s %s", num, unit);
    }
    else
    {
        unit = "MB";
        size = bytes / 1e6;
        if (size >= 1000)
        {
            unit = "GB";
            size /= 1000;
        }
        if (size >= 1000)
        {
            unit = "TB";
            size /= 1000;
        }
        snprintf(out, out_size, "%.1f %s", size, unit);
    }
}
