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

#if defined(DEBUG_API) || defined(DEBUG_API_OEM)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

static uint16_t oem_code_page_run;
static uint16_t oem_auto_cp;

// Resolve the code page to apply: the override if set, else the locale auto.
static uint16_t oem_resolve(void)
{
    return oem_get_code_page() ? oem_get_code_page() : oem_auto_cp;
}

static void oem_request_code_page(uint16_t cp)
{
    uint16_t old_code_page = oem_code_page_run;
    // cp >= 900 are DBCS; allow SBCS only
    if (cp < 900 && unicode_has_page(cp))
    {
        oem_fs_code_page(cp);
        oem_code_page_run = cp;
    }
    if (old_code_page != oem_code_page_run)
        vga_set_code_page(oem_code_page_run);
}

void HOST_IN_FLASH("oem_init") oem_init(void)
{
    oem_apply_code_page(oem_get_code_page(), true);
    /* The glyph store was rebuilt by font_init just above and has forgotten
     * the page; oem_request_code_page only speaks when the number changes, so
     * it would stay forgotten. On a machine whose font is another chip this is
     * the PIX message that tells it, and this is where that message goes. */
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

/* Zero is auto: track whatever the locale's default is. A page is carried
 * or it is not, and the table says which without applying anything -- the
 * same test oem_request_code_page makes before it speaks. */
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

/* SET's line for this row. Auto reports the page it landed on. */
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

int oem_to_utf8_char(unsigned char b, char *dst)
{
    return unicode_to_utf8_char(b, oem_code_page_run, dst);
}

// Truncation never splits a sequence: once one doesn't fit, writing stops
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
