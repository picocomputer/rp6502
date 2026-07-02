/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Integration test for the RIA clock/time API (clk.c). rtc.rp6502 prints the
 * current time plus two fixed timestamps through gmtime/localtime/strftime, so
 * with TZ pinned to UTC the fixed lines are deterministic and exercise the
 * whole syscall chain (ops 0x3A/0x3B/0x3D/0x3F). Two more tests drive strftime
 * directly to prove the UTF-8 -> OEM code-page conversion (FatFs ff_uni2oem)
 * and that %z reflects the host timezone offset.
 */

#include "emu/api/api.h"
#include "emu/api/clk.h"
#include "emu/api/oem.h"
#include "emu/sys/com.h"
#include "emu/mon/rom.h"
#include "emu/sys/mem.h"
#include "emu/chips/rp6502.h"
#include "emu/sys/sys.h"
#include "api/api.h"
#include "api/clk.h"
#include "utest.h"
#include <stdlib.h>
#include <string.h>

static char cap[1 << 16];
static size_t cap_len;

static void tap(const char *buf, int len)
{
    for (int i = 0; i < len && cap_len < sizeof(cap) - 1; i++)
        cap[cap_len++] = buf[i];
    cap[cap_len] = 0;
}

static void run_frames(int n)
{
    for (int i = 0; i < n; i++)
        emu_run_frame();
}

/* The 18-byte wire tm the 6502 libc pushes: 9 int16 in struct-tm order. */
struct wire_tm
{
    int16_t sec, min, hour, mday, mon, year, wday, yday, isdst;
};

/* Drive the strftime syscall the way the 6502 libc does (18-byte tm on top,
 * then NUL, then format) and copy the OEM result back. Returns its length. */
static uint16_t drive_strftime(const struct wire_tm *w, const char *fmt,
                               char *out, size_t outsz)
{
    size_t flen = strlen(fmt);
    memcpy(&xstack[XSTACK_SIZE - 18], w, 18);
    xstack[XSTACK_SIZE - 19] = 0;
    memcpy(&xstack[XSTACK_SIZE - 19 - flen], fmt, flen);
    xstack_ptr = (uint16_t)(XSTACK_SIZE - 19 - flen);
    clk_api_strftime();
    uint16_t n = (uint16_t)(API_A | (API_X << 8));
    size_t i = 0;
    for (; i < n && i + 1 < outsz; i++)
        out[i] = (char)xstack[xstack_ptr + i];
    out[i] = 0;
    return n;
}

/* With TZ=UTC the local lines equal the UTC lines, so the two fixed timestamps
 * (1-Jan-2025 and 1-Jul-2025, both noon UTC) render deterministically. */
UTEST(rtc, prints_fixed_timestamps)
{
    setenv("TZ", "UTC", 1);
    setenv("LC_ALL", "C", 1);
    cap_len = 0;
    cap[0] = 0;
    ASSERT_TRUE(emu_rom_load(RTC_ROM));
    emu_init();
    com_set_stdout_tap(tap);
    run_frames(120);
    com_set_stdout_tap(NULL);

    ASSERT_TRUE(emu_cpu_halted); /* program runs to completion */
    ASSERT_TRUE(strstr(cap, "Jan") != NULL);
    ASSERT_TRUE(strstr(cap, "Jul") != NULL);
    ASSERT_TRUE(strstr(cap, "12:00:00 2025") != NULL);
    ASSERT_TRUE(strstr(cap, "UTC") != NULL); /* the %Z timezone name */
}

/* strftime copies literal format bytes verbatim, so an embedded UTF-8 "é"
 * (0xC3 0xA9) must come back as the active code page's glyph — CP437 0x82 —
 * proving clk.c routes strftime output through the OEM converter. */
UTEST(rtc, strftime_maps_utf8_to_oem)
{
    setenv("LC_ALL", "C", 1);
    ASSERT_TRUE(emu_rom_load(RTC_ROM));
    emu_init(); /* resets clk (locale) and atr (code page 437) */

    struct wire_tm w = {0, 0, 12, 1, 0, 125, 3, 0, 0}; /* fields unused by literals */
    char out[16];
    uint16_t n = drive_strftime(&w, "caf\xC3\xA9", out, sizeof out); /* "café" UTF-8 */
    ASSERT_EQ(n, (uint16_t)4);              /* 'c' 'a' 'f' + one OEM byte */
    ASSERT_EQ((unsigned char)out[3], 0x82); /* CP437 'é' */
    ASSERT_EQ(out[0], 'c');
}

/* %z must reflect the host timezone offset, not glibc's +0000 default for a
 * tm without tm_gmtoff. PST8 is a POSIX TZ (UTC-8, no DST) needing no tzdata. */
UTEST(rtc, strftime_z_uses_host_offset)
{
    setenv("TZ", "PST8", 1);
    setenv("LC_ALL", "C", 1);
    ASSERT_TRUE(emu_rom_load(RTC_ROM));
    emu_init(); /* clk_reset -> tzset() adopts TZ=PST8 */

    struct wire_tm w = {0, 0, 12, 1, 6, 125, 2, 181, 0}; /* 2025-07-01, no DST */
    char out[16];
    drive_strftime(&w, "%z", out, sizeof out);
    ASSERT_STREQ(out, "-0800");
}

/* The active code page (oem.c) drives the OEM conversion: 'ã' (U+00E3) is
 * unmappable in the default CP437 (-> 0x7F) but is 0xC6 in CP850. Switching the
 * page changes the strftime output, and unsupported pages are rejected. */
UTEST(rtc, code_page_drives_oem_mapping)
{
    setenv("LC_ALL", "C", 1);
    ASSERT_TRUE(emu_rom_load(RTC_ROM));
    emu_init();
    ASSERT_EQ(oem_get_code_page(), (uint16_t)437); /* default */

    struct wire_tm w = {0, 0, 12, 1, 0, 125, 3, 0, 0};
    char out[8];
    drive_strftime(&w, "\xC3\xA3", out, sizeof out); /* "ã" UTF-8 */
    ASSERT_EQ((unsigned char)out[0], 0x7F);          /* not in CP437 */

    ASSERT_FALSE(oem_set_code_page(999));          /* unsupported: rejected */
    ASSERT_EQ(oem_get_code_page(), (uint16_t)437); /* unchanged */

    ASSERT_TRUE(oem_set_code_page(850));
    ASSERT_EQ(oem_get_code_page(), (uint16_t)850);
    drive_strftime(&w, "\xC3\xA3", out, sizeof out);
    ASSERT_EQ((unsigned char)out[0], 0xC6); /* CP850 'ã' */
}

/* An exec'd program inherits the current code page (it reads it back via the
 * attribute), so the exec-reload reset (ria_reset) must NOT revert it — else the
 * font (untouched by exec) and the code page would desync. */
UTEST(rtc, exec_preserves_code_page)
{
    ASSERT_TRUE(emu_rom_load(RTC_ROM));
    emu_init();
    ASSERT_EQ(oem_get_code_page(), (uint16_t)437);
    ASSERT_TRUE(oem_set_code_page(850));
    ria_reset(); /* the path the exec reload runs */
    ASSERT_EQ(oem_get_code_page(), (uint16_t)850); /* preserved across exec */
}

/* A cold boot (emu_init), unlike exec, resets the code page to the 437 default. */
UTEST(rtc, cold_boot_defaults_code_page)
{
    ASSERT_TRUE(emu_rom_load(RTC_ROM));
    emu_init();
    ASSERT_TRUE(oem_set_code_page(850));
    emu_init(); /* cold boot */
    ASSERT_EQ(oem_get_code_page(), (uint16_t)437);
}

UTEST_MAIN()
