/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/oem.h"
#include "str/str.h"
#include "sys/cfg.h"
#include "sys/cpu.h"
#include <fatfs/ff.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <pico.h>
#include <pico/printf.h>
#include <sys/cdefs.h>

#if defined(DEBUG_RIA_STR) || defined(DEBUG_RIA_STR_STR)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

static_assert(CPU_PHI2_MIN_KHZ >= 0); // catch missing include
#define STR_PHI2_MIN_MAX __XSTRING(CPU_PHI2_MIN_KHZ) "-" __XSTRING(CPU_PHI2_MAX_KHZ)
#define STR_OEM_CODE_PAGE __XSTRING(RP6502_CODE_PAGE)

// Non-localized string literals: flash, or RAM with XR().
#define X(name, value) \
    const char __in_flash(__XSTRING(name)) name[] = value;
#define XR(name, value) \
    const char __not_in_flash(__XSTRING(name)) name[] = value;
#include "str.def"
#undef X
#undef XR

// Localized strings, one storage copy per locale (suffixed _en/_pl). X
// stays in flash; XR is RAM-resident so it survives the XIP suspend during
// flashing. This is a copy_to_ram build, so plain literals would land in
// RAM -- keep each string explicitly __in_flash.
#define X(name, value) static const char __in_flash(__XSTRING(name)) name##_en[] = value;
#define XR(name, value) static const char __not_in_flash(__XSTRING(name)) name##_en[] = value;
#include "str_en.def"
#undef X
#undef XR
#define X(name, value) static const char __in_flash(__XSTRING(name)) name##_pl[] = value;
#define XR(name, value) static const char __not_in_flash(__XSTRING(name)) name##_pl[] = value;
#include "str_pl.def"
#undef X
#undef XR

// Per-locale lookup tables (in flash), indexed by enum str_loc_id.
#define X(name, value) name##_en,
#define XR(name, value) name##_en,
static const char *const __in_flash("str_loc_en") str_loc_en[] = {
#include "str_en.def"
};
#undef X
#undef XR
#define X(name, value) name##_pl,
#define XR(name, value) name##_pl,
static const char *const __in_flash("str_loc_pl") str_loc_pl[] = {
#include "str_pl.def"
};
#undef X
#undef XR

// Locale registry. The string tables and these parallel arrays are all
// ordered by RP6502_LOCALES, so one index selects the table, the names and
// the default code page. This module is the sole owner of the active locale.
#define X(suffix, file, shortn, verbose, cp) str_loc_##file,
static const char *const *const __in_flash("str_tables") str_tables[] = {
    RP6502_LOCALES};
#undef X
#define X(suffix, file, shortn, verbose, cp) shortn,
static const char *const __in_flash("str_locale_names") str_locale_names[] = {
    RP6502_LOCALES};
#undef X
#define X(suffix, file, shortn, verbose, cp) verbose,
static const char *const __in_flash("str_locale_verbose") str_locale_verbose[] = {
    RP6502_LOCALES};
#undef X
#define X(suffix, file, shortn, verbose, cp) cp,
static const uint16_t __in_flash("str_locale_cp") str_locale_cp[] = {
    RP6502_LOCALES};
#undef X

static_assert(sizeof str_loc_pl == sizeof str_loc_en,
              "locale string tables differ in length");

static int str_locale_index;
static bool str_locale_loaded;

const char *S(int id)
{
    return str_tables[str_locale_index][id];
}

// Switch the active string table (clamped). Internal; the locale is selected
// by name through str_set_locale / str_load_locale.
static void str_select_locale(int index)
{
    int count = (int)(sizeof str_tables / sizeof str_tables[0]);
    str_locale_index = (index >= 0 && index < count) ? index : 0;
}

// Find a locale by short name. Falls back to the build default
// (RP6502_LOCALE) when name is empty or unknown, mirroring kbd.
static int str_sanitize_locale(const char *name)
{
    const int count = sizeof(str_locale_names) / sizeof(str_locale_names)[0];
    int default_index = 0;
    int found_index = -1;
    for (int i = 0; i < count; i++)
    {
        if (!strcasecmp(str_locale_names[i], __XSTRING(RP6502_LOCALE)))
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

void str_init(void)
{
    if (!str_locale_loaded)
        str_apply_locale(str_sanitize_locale(""));
}

int str_locales_response(char *buf, size_t buf_size, int state)
{
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
    {
        str_apply_locale(new_index);
        cfg_save();
    }
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
        if (str_is_sep(*segs_src))
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
        if (str_is_sep(*segs_src))
            segs_src++;
    }

    // For relative paths start at the end of the CWD already in str_buf.
    // For absolute paths start fresh after the drive prefix.
    size_t out;
    if (!strchr(path, ':') && !str_is_sep(path[0]))
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
        const char *next = seg;
        while (*next && !str_is_sep(*next))
            next++;
        size_t slen = (size_t)(next - seg);
        if (!*next)
            next = NULL;
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
            if (drive_len == 1) // ":" installed ROM
                for (size_t k = 0; k < slen; k++)
                    str_buf[out++] = (char)toupper((unsigned char)seg[k]);
            else
            {
                str_buf[out++] = '/';
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

// Case-insensitive compare matching FatFs's name lookup: convert each
// OEM byte to Unicode via the configured code page, then upper-case via
// ff_wtoupper. strcasecmp would only handle ASCII.
static bool str_lfn_eq(const char *a, const char *b)
{
    for (;;)
    {
        WCHAR ua = ff_oem2uni((unsigned char)*a, FF_CODE_PAGE);
        WCHAR ub = ff_oem2uni((unsigned char)*b, FF_CODE_PAGE);
        if (ff_wtoupper(ua) != ff_wtoupper(ub))
            return false;
        if (!*a)
            return true;
        a++;
        b++;
    }
}

bool str_correct_basename(char *path, size_t path_size)
{
    char fname[FF_LFN_BUF + 1];
    if (!str_lookup_basename(path, fname, sizeof fname))
        return true; // lookup failed; leave the input case unchanged

    // Find where the basename starts (after the last separator or drive
    // colon), ignoring any trailing separators on path.
    size_t end = strlen(path);
    while (end > 0 && str_is_sep(path[end - 1]))
        end--;

    size_t name_start = 0;
    const char *colon = strchr(path, ':');
    if (colon && (size_t)(colon - path) < end)
        name_start = (size_t)(colon - path) + 1;
    for (size_t i = name_start; i < end; i++)
        if (str_is_sep(path[i]))
            name_start = i + 1;

    size_t fname_len = strlen(fname);
    if (name_start + fname_len + 1 > path_size)
        return false;
    memcpy(path + name_start, fname, fname_len + 1);
    return true;
}

bool str_lookup_basename(const char *path, char *out, size_t out_size)
{
    if (out_size)
        out[0] = '\0';

    // Trim trailing separators so "folder/" is treated like "folder".
    size_t end = strlen(path);
    while (end > 0 && str_is_sep(path[end - 1]))
        end--;
    if (end == 0)
        return false;

    // Find basename. ':' caps the drive prefix; treat the byte after it
    // as the start of the path so "X:foo" splits as parent "X:" + name "foo".
    size_t name_start = 0;
    const char *colon = strchr(path, ':');
    if (colon)
        name_start = (size_t)(colon - path) + 1;
    for (size_t i = name_start; i < end; i++)
        if (str_is_sep(path[i]))
            name_start = i + 1;

    size_t name_len = end - name_start;
    if (name_len == 0 || name_len > FF_LFN_BUF)
        return false;

    char name[FF_LFN_BUF + 1];
    memcpy(name, path + name_start, name_len);
    name[name_len] = '\0';

    // Build parent. Drop a trailing '/' or '\' unless we'd be left with
    // a bare drive prefix ("X:") or the root separator alone ("/"), in
    // which case f_opendir needs the separator kept.
    char parent[FF_LFN_BUF + 1];
    if (name_start == 0)
    {
        parent[0] = '.';
        parent[1] = '\0';
    }
    else
    {
        size_t parent_len = name_start;
        if (parent_len > 1 && str_is_sep(path[parent_len - 1]) &&
            path[parent_len - 2] != ':')
            parent_len--;
        if (parent_len >= sizeof parent)
            return false;
        memcpy(parent, path, parent_len);
        parent[parent_len] = '\0';
    }

    DIR dir;
    if (f_opendir(&dir, parent) != FR_OK)
        return false;
    bool found = false;
    FILINFO fno;
    for (;;)
    {
        if (f_readdir(&dir, &fno) != FR_OK || !fno.fname[0])
            break;
        if (str_lfn_eq(fno.fname, name))
        {
            size_t flen = strlen(fno.fname);
            if (flen + 1 <= out_size)
            {
                memcpy(out, fno.fname, flen + 1);
                found = true;
            }
            break;
        }
    }
    f_closedir(&dir);
    return found;
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

unsigned char str_utf8_to_oem(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    unsigned char b0 = *s;
    if (!b0)
        return 0;
    if (b0 < 0x80)
    {
        *p = (const char *)(s + 1);
        return b0;
    }
    uint32_t cp;
    int extra;
    if ((b0 & 0xE0) == 0xC0)
    {
        cp = b0 & 0x1F;
        extra = 1;
    }
    else if ((b0 & 0xF0) == 0xE0)
    {
        cp = b0 & 0x0F;
        extra = 2;
    }
    else if ((b0 & 0xF8) == 0xF0)
    {
        cp = b0 & 0x07;
        extra = 3;
    }
    else
    {
        *p = (const char *)(s + 1);
        return 0x7F;
    }
    for (int i = 1; i <= extra; i++)
    {
        unsigned char bi = s[i];
        if ((bi & 0xC0) != 0x80)
        {
            *p = (const char *)(s + i);
            return 0x7F;
        }
        cp = (cp << 6) | (bi & 0x3F);
    }
    *p = (const char *)(s + extra + 1);
    if (cp >= 0xD800 && cp <= 0xDFFF)
        return 0x7F;
    WCHAR oem = ff_uni2oem(cp, oem_get_code_page_run());
    return oem ? (unsigned char)oem : 0x7F;
}

// Per-call UTF-8 decode state used by the *_utf8 vfctprintf callbacks.
// One byte in per callback invocation; one OEM byte out per complete
// codepoint. Buffer fields are unused by the putchar variant.
typedef struct
{
    uint32_t accum;       // partial codepoint
    uint8_t continuation; // continuation bytes still expected
    char *dst;
    size_t dst_size;      // total dst capacity (incl. NUL)
    size_t bytes_written; // OEM bytes written or would-be-written
} utf8_state;

static unsigned char utf8_codepoint_to_oem(uint32_t cp)
{
    if (cp >= 0xD800 && cp <= 0xDFFF)
        return 0x7F;
    WCHAR w = ff_uni2oem(cp, oem_get_code_page_run());
    return w ? (unsigned char)w : 0x7F;
}

static void utf8_emit_buf(utf8_state *st, unsigned char oem)
{
    if (st->dst_size && st->bytes_written + 1 < st->dst_size)
        st->dst[st->bytes_written] = (char)oem;
    st->bytes_written++;
}

#define UTF8_FEED(st, b, EMIT)                              \
    do                                                      \
    {                                                       \
        unsigned char _b = (unsigned char)(b);              \
        if (_b < 0x80)                                      \
        {                                                   \
            if ((st)->continuation)                         \
            {                                               \
                EMIT(0x7F);                                 \
                (st)->continuation = 0;                     \
            }                                               \
            EMIT(_b);                                       \
        }                                                   \
        else if ((_b & 0xC0) == 0x80)                       \
        {                                                   \
            if (!(st)->continuation)                        \
            {                                               \
                EMIT(0x7F);                                 \
                break;                                      \
            }                                               \
            (st)->accum = ((st)->accum << 6) | (_b & 0x3F); \
            if (--(st)->continuation == 0)                  \
                EMIT(utf8_codepoint_to_oem((st)->accum));   \
        }                                                   \
        else                                                \
        {                                                   \
            if ((st)->continuation)                         \
                EMIT(0x7F);                                 \
            if ((_b & 0xE0) == 0xC0)                        \
            {                                               \
                (st)->accum = _b & 0x1F;                    \
                (st)->continuation = 1;                     \
            }                                               \
            else if ((_b & 0xF0) == 0xE0)                   \
            {                                               \
                (st)->accum = _b & 0x0F;                    \
                (st)->continuation = 2;                     \
            }                                               \
            else if ((_b & 0xF8) == 0xF0)                   \
            {                                               \
                (st)->accum = _b & 0x07;                    \
                (st)->continuation = 3;                     \
            }                                               \
            else                                            \
            {                                               \
                EMIT(0x7F);                                 \
                (st)->continuation = 0;                     \
            }                                               \
        }                                                   \
    } while (0)

static void cb_putchar(char c, void *arg)
{
    utf8_state *st = (utf8_state *)arg;
#define EMIT(x) putchar((x))
    UTF8_FEED(st, c, EMIT);
#undef EMIT
}

static void cb_buf(char c, void *arg)
{
    utf8_state *st = (utf8_state *)arg;
#define EMIT(x) utf8_emit_buf(st, (x))
    UTF8_FEED(st, c, EMIT);
#undef EMIT
}

int vprintf_utf8(const char *utf8_fmt, va_list va)
{
    utf8_state st = {0};
    int n = vfctprintf(cb_putchar, &st, utf8_fmt, va);
    if (st.continuation)
        putchar(0x7F);
    return n;
}

int printf_utf8(const char *utf8_fmt, ...)
{
    va_list va;
    va_start(va, utf8_fmt);
    int n = vprintf_utf8(utf8_fmt, va);
    va_end(va);
    return n;
}

int vsnprintf_utf8(char *dst, size_t dst_size,
                   const char *utf8_fmt, va_list va)
{
    utf8_state st = {0};
    st.dst = dst;
    st.dst_size = dst_size;
    (void)vfctprintf(cb_buf, &st, utf8_fmt, va);
    if (st.continuation)
        utf8_emit_buf(&st, 0x7F);
    if (dst_size)
    {
        size_t end = st.bytes_written < dst_size ? st.bytes_written : dst_size - 1;
        dst[end] = 0;
    }
    return (int)st.bytes_written;
}

int snprintf_utf8(char *dst, size_t dst_size, const char *utf8_fmt, ...)
{
    va_list va;
    va_start(va, utf8_fmt);
    int n = vsnprintf_utf8(dst, dst_size, utf8_fmt, va);
    va_end(va);
    return n;
}
