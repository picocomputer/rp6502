/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/str/oem.h"
#include "core/str/str.h"
#include "core/sys/config.h"
#include "core/wdc/phi2.h"
/* FatFs where there is one: ff.h declares ff_oem2uni and ff_wtoupper itself,
 * in types it picks per platform, and it is the authority wherever a tree has
 * it. core/str/unicode.h declares them for a tree that does not -- the
 * Pocket, which has no FatFs anywhere. */
#ifdef __has_include
#if __has_include(<fatfs/ff.h>)
#include <fatfs/ff.h>
#endif
#endif
#include "core/str/unicode.h"
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdio.h>
#include "machine.h"
#include <assert.h>

#if defined(DEBUG_STR) || defined(DEBUG_STR_STR)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

/* Two-level so an argument that is itself a macro expands first, which is
 * the whole reason these are here: glibc's __CONCAT expands once. */
#define STR_CAT1(a, b) a##b
#define STR_CAT(a, b) STR_CAT1(a, b)

static_assert(PHI2_MIN_KHZ >= 0); // catch missing include
#define STR_PHI2_MIN_MAX STR_XSTR(PHI2_MIN_KHZ) "-" STR_XSTR(PHI2_MAX_KHZ)

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

const char *S(int id)
{
    return str_tabs[str_locale_index][id];
}

// Switch the active string table (clamped). Internal; the locale is selected
// by name; str_check_locale is what judges one.
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

/* The file keeps the canonical spelling, so "en" is stored as "EN". An
 * unknown name is not sanitized here the way loading once did -- a name
 * that is not a locale is not a locale. */
bool str_check_locale(const char *in, char *out)
{
    int i = str_sanitize_locale(in);
    if (strcasecmp(in, str_locale_names[i]))
        return false;
    strcpy(out, str_locale_names[i]);
    return true;
}

/* Switch the string table and push the locale's default code page to oem
 * (oem only acts on it in auto mode). */
void str_apply_locale(const char *name, bool changed)
{
    (void)changed;
    int index = str_sanitize_locale(name);
    str_select_locale(index);
    oem_locale_changed(str_locale_cp[index]);
}

void HOST_IN_FLASH("str_init") str_init(void)
{
    str_apply_locale(str_get_locale(), true);
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

/* SET's line for this row, now that the row is this driver's. */
int str_locale_response(char *buf, size_t buf_size, int state, unsigned width)
{
    (void)state;
    (void)width;
    snprintf(buf, buf_size, STR_SET_LOC_RESPONSE,
             str_get_locale(), str_get_locale_verbose());
    return -1;
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
    uint16_t cp = oem_get_code_page_run();
    for (;;)
    {
        uint16_t ua = ff_oem2uni((unsigned char)*a, cp);
        uint16_t ub = ff_oem2uni((unsigned char)*b, cp);
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
    if (bytes < 5000000ULL)
    {
        // Floppy-era media: KB, rolling to MB, trailing zeros stripped.
        unsigned milli;
        if (bytes < 1024000ULL)
        {
            unit = "KB";
            milli = (unsigned)((bytes * 1000 + 512) / 1024);
        }
        else
        {
            unit = "MB";
            milli = (unsigned)((bytes + 512) / 1024);
        }
        char num[16];
        snprintf(num, sizeof(num), "%u.%03u", milli / 1000, milli % 1000);
        char *p = num + strlen(num) - 1;
        while (*p == '0')
            *p-- = '\0';
        if (*p == '.')
            *p = '\0';
        snprintf(out, out_size, "%s %s", num, unit);
    }
    else
    {
        unsigned tenths;
        if (bytes < 1000000000ULL)
        {
            unit = "MB";
            tenths = (unsigned)((bytes + 50000) / 100000);
        }
        else if (bytes < 1000000000000ULL)
        {
            unit = "GB";
            tenths = (unsigned)((bytes + 50000000) / 100000000);
        }
        else
        {
            unit = "TB";
            tenths = (unsigned)((bytes + 50000000000ULL) / 100000000000ULL);
        }
        snprintf(out, out_size, "%u.%u %s", tenths / 10, tenths % 10, unit);
    }
}
