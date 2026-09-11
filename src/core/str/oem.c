/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "osal/dir.h"
#include "core/str/oem.h"
#include "core/sys/config.h"
#include "core/str/str.h"
#include "core/vga/vga.h"
#include "core/str/unicode.h"
#include "machine.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static uint16_t oem_code_page_run;
static uint16_t oem_auto_cp;

static uint16_t oem_resolve(void)
{
    return oem_get_code_page() ? oem_get_code_page() : oem_auto_cp;
}

static void oem_request_code_page(uint16_t cp)
{
    uint16_t old_code_page = oem_code_page_run;
    if (cp < 900 && unicode_has_page(cp))
    {
        oem_fs_code_page(cp);
        oem_code_page_run = cp;
    }
    if (old_code_page != oem_code_page_run)
        vga_set_code_page(oem_code_page_run);
}

void oem_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put_u16(c, oem_code_page_run);
}

bool oem_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    uint16_t cp = sst_get_u16(c);
    /* Zero is a valid saved value, because a machine whose resolved page the
     * tables do not carry runs with no page at all. */
    if (!sst_ok(c) || (cp != 0 && (cp >= 900 || !unicode_has_page(cp))))
        return false;
    oem_code_page_run = cp;
    oem_fs_code_page(cp);
    /* vga_load_code_page rather than vga_set_code_page, because choosing a
     * page resets the terminal and the savestate has already put the
     * terminal's cells back by the time this runs. */
    vga_load_code_page(cp);
    return true;
}

void HOST_IN_FLASH("oem_init") oem_init(void)
{
    oem_apply_code_page(oem_get_code_page(), true);
    /* oem_request_code_page calls vga_set_code_page only when the number
     * changes, so the display is given a page here even when it did not. */
    vga_set_code_page(oem_code_page_run);
}

void oem_stop(void)
{
    if (oem_code_page_run != oem_resolve())
        oem_request_code_page(oem_resolve());
}

void oem_set_code_page_run(uint16_t cp)
{
    oem_request_code_page(cp);
}

/* Zero is auto: follow the locale's default. */
bool oem_check_code_page(uint16_t *v)
{
    return *v == 0 || (*v < 900 && unicode_has_page(*v));
}

void oem_apply_code_page(uint16_t cp, bool changed)
{
    (void)cp;
    (void)changed;
    oem_request_code_page(oem_resolve());
}

bool oem_is_auto(void)
{
    return oem_get_code_page() == 0;
}

int oem_code_page_response(char *buf, size_t buf_size, int state, unsigned width)
{
    (void)state;
    (void)width;
    if (oem_is_auto())
        oem_snprintf(buf, buf_size, STR_SET_CODE_PAGE_AUTO_RESPONSE, oem_get_code_page_run());
    else
        oem_snprintf(buf, buf_size, STR_SET_CODE_PAGE_RESPONSE, oem_get_code_page());
    return -1;
}

uint16_t oem_get_code_page_run(void)
{
    return oem_code_page_run;
}

void oem_locale_changed(uint16_t cp)
{
    oem_auto_cp = cp;
    if (oem_is_auto())
        oem_request_code_page(oem_resolve());
}

unsigned char oem_from_codepoint(uint32_t cp)
{
    return unicode_from_codepoint(cp, oem_code_page_run);
}

unsigned char oem_from_utf8_next(const char **p)
{
    return unicode_from_utf8_next(p, oem_code_page_run);
}

static size_t oem_utf8_len(unsigned char lead)
{
    if ((lead & 0xe0) == 0xc0)
        return 2;
    if ((lead & 0xf0) == 0xe0)
        return 3;
    if ((lead & 0xf8) == 0xf0)
        return 4;
    return 1;
}

size_t oem_from_utf8_run(oem_run_t *run, const char *utf8, size_t len, bool end,
                         char *dst, size_t dstsz, size_t *taken)
{
    size_t in = 0, out = 0;
    while (in < len && out < dstsz)
    {
        unsigned char c = (unsigned char)utf8[in];
        bool paired = run->after_cr;
        run->after_cr = false;
        if (c == '\r' || c == '\n')
        {
            /* A carriage return is written at once rather than held to see
             * whether a line feed follows, because the run carries state and
             * not output: a held return would have nothing to flush it. */
            if (c == '\n' && paired)
            {
                in++; /* the second half of a CRLF the last call cut */
                continue;
            }
            dst[out++] = '\r';
            in++;
            if (c != '\r')
                continue;
            if (in < len)
            {
                if (utf8[in] == '\n')
                    in++;
            }
            else
                run->after_cr = true; /* its line feed may open the next call */
            continue;
        }
        if (c < 0x80)
        {
            dst[out++] = (char)c;
            in++;
            continue;
        }
        size_t n = oem_utf8_len(c);
        if (n > len - in)
        {
            if (!end)
                break; /* the rest of the sequence has not arrived */
            n = len - in;
        }
        char seq[5];
        memcpy(seq, utf8 + in, n);
        seq[n] = 0;
        const char *p = seq;
        unsigned char b = oem_from_utf8_next(&p);
        dst[out++] = (char)(b && b != 0x7F ? b : '?');
        in += n;
    }
    if (taken)
        *taken = in;
    return out;
}

int oem_to_utf8_char(unsigned char b, char *dst)
{
    return unicode_to_utf8_char(b, oem_code_page_run, dst);
}

// Truncation never splits a sequence: once one does not fit, writing stops
// but the needed length keeps counting.
size_t oem_to_utf8(const char *s, char *dst, size_t dstsz)
{
    size_t need = 0;
    size_t out = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
    {
        char enc[3];
        size_t n = (size_t)oem_to_utf8_char(*p, enc);
        if (dstsz && out == need && need + n < dstsz)
        {
            memcpy(dst + out, enc, n);
            out += n;
        }
        need += n;
    }
    if (dstsz)
        dst[out] = 0;
    return need;
}

size_t oem_from_utf8(const char *u8, char *dst, size_t dstsz)
{
    size_t need = 0;
    const char *p = u8;
    unsigned char b;
    while ((b = oem_from_utf8_next(&p)))
    {
        if (dstsz && need + 1 < dstsz)
            dst[need] = (char)b;
        need++;
    }
    if (dstsz)
        dst[need < dstsz ? need : dstsz - 1] = 0;
    return need;
}

int oem_to_wide(const char *s, uint16_t *w, int wcount)
{
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && n < wcount - 1; p++)
    {
        uint16_t u = *p < 0x80 ? *p : ff_oem2uni(*p, oem_code_page_run);
        w[n++] = u ? u : 0xFFFD;
    }
    if (wcount > 0)
        w[n] = 0;
    return n;
}

size_t oem_from_wide_n(const uint16_t *w, size_t wlen, char *dst, size_t dstsz)
{
    size_t n = 0;
    for (size_t i = 0; i < wlen && n + 1 < dstsz; i++)
    {
        unsigned char b = w[i] < 0x80 ? (unsigned char)w[i]
                                      : (unsigned char)ff_uni2oem(w[i], oem_code_page_run);
        dst[n++] = b ? (char)b : 0x7F;
    }
    if (dstsz)
        dst[n] = 0;
    return n;
}

size_t oem_from_wide(const uint16_t *w, char *dst, size_t dstsz)
{
    size_t len = 0;
    while (w[len])
        len++;
    return oem_from_wide_n(w, len, dst, dstsz);
}

/* 0x7F is what the conversions put where a character had no spelling, and
 * FatFs rejects 0x7F in a name outright, so a result of 0x7F means "no
 * spelling" whichever way it got there. */
#define OEM_NO_SPELLING 0x7F

bool oem_maps_utf8(const char *u8)
{
    const char *p = u8;
    while (*p)
        if (oem_from_utf8_next(&p) == OEM_NO_SPELLING)
            return false;
    return true;
}

bool oem_maps_wide(const uint16_t *w)
{
    for (; *w; w++)
        if (*w == OEM_NO_SPELLING ||
            (*w >= 0x80 && !ff_uni2oem(*w, oem_code_page_run)))
            return false;
    return true;
}

bool oem_maps_oem(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p >= 0x80 && !ff_oem2uni(*p, oem_code_page_run))
            return false;
    return true;
}

int oem_vsnprintf(char *dst, size_t dst_size, const char *utf8_fmt, va_list va)
{
    vsnprintf(dst, dst_size, utf8_fmt, va);
    return (int)oem_from_utf8(dst, dst, dst_size);
}

int oem_snprintf(char *dst, size_t dst_size, const char *utf8_fmt, ...)
{
    va_list va;
    va_start(va, utf8_fmt);
    int n = oem_vsnprintf(dst, dst_size, utf8_fmt, va);
    va_end(va);
    return n;
}
