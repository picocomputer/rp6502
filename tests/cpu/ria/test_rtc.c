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

#include "core/sys/config.h"
#include "core/api/api.h"
#include "core/api/clk.h"
#include "core/sys/sys.h"
#include "core/str/oem.h"
#include "core/com/com.h"
#include "core/ria/regs.h"
#include "core/wdc/resb.h"
#include "tb_hostos.h"
#include "emu_boot.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    emu_frames((int)n);
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
    host_setenv("TZ", "UTC");
    tzset(); /* adopt the TZ live; sys_init's one tzset ran with the host default */
    cap_len = 0;
    cap[0] = 0;
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));
    com_set_tx_tap(tap);
    run_frames(120);
    com_set_tx_tap(NULL);

    ASSERT_FALSE(resb_running()); /* program runs to completion */
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
    oem_set_code_page_run(437); /* the mapping under test (drive_strftime needs no ROM) */

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
    host_setenv("TZ", "PST8");
    tzset(); /* adopt PST8 live (drive_strftime needs no ROM) */

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
    oem_set_code_page_run(437);
    ASSERT_EQ(oem_get_code_page_run(), (uint16_t)437);

    struct wire_tm w = {0, 0, 12, 1, 0, 125, 3, 0, 0};
    char out[8];
    drive_strftime(&w, "\xC3\xA3", out, sizeof out); /* "ã" UTF-8 */
    ASSERT_EQ((unsigned char)out[0], 0x7F);          /* not in CP437 */

    ASSERT_FALSE(oem_set_code_page(999));              /* unsupported: rejected, run unchanged */
    ASSERT_EQ(oem_get_code_page_run(), (uint16_t)437); /* unchanged */

    oem_set_code_page_run(850); /* guest runtime change (leaves the config override clear) */
    ASSERT_EQ(oem_get_code_page_run(), (uint16_t)850);
    drive_strftime(&w, "\xC3\xA3", out, sizeof out);
    ASSERT_EQ((unsigned char)out[0], 0xC6); /* CP850 'ã' */
}

/* A run-only code page belongs to the run that set it. The stop every program
 * change goes through puts it back, which is oem_stop in the firmware's stop
 * fan-out; the font cannot desync from it, because the revert goes out over the
 * same vga_set_code_page the change did. */
UTEST(rtc, stop_reverts_run_code_page)
{
    /* Stop once to shed whatever run page an earlier case left: a program's
     * own exit parks the drivers now, so a restart's stop finds nothing to
     * do and this case has to make its own starting point. */
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));
    sys_stop();
    sys_commit();
    const uint16_t resolved = oem_get_code_page_run(); /* the config's, or the locale's */

    ASSERT_TRUE(emu_restart(TEST_FIXTURE));
    const uint16_t guest = resolved == 850 ? 437 : 850;
    oem_set_code_page_run(guest); /* a guest program changed the run page */
    ASSERT_EQ(oem_get_code_page_run(), guest);
    sys_stop();
    sys_commit();
    ASSERT_EQ(oem_get_code_page_run(), resolved);
}

/* The host's clock is the host's. Only a machine that owns a real time-of-day
 * clock -- the Pico, with its always-on timer -- may move it; an emulator has
 * no business rewriting the wall its user is living on, and a machine whose
 * time was handed to it at boot has nowhere to write one back. Both refuse,
 * and refusing is EACCES rather than a range complaint. */
UTEST(rtc, settime_is_refused_on_a_machine_that_does_not_own_the_clock)
{
    api_set_errno_opt(2); /* llvm-mos mapping, so API_ERRNO is decodable */
    const int64_t before = (int64_t)time(NULL);
    const int64_t want = 1735732800; /* 2025-01-01 noon UTC */
    memcpy(&xstack[XSTACK_SIZE - 8], &want, 8);
    xstack_ptr = XSTACK_SIZE - 8;
    clk_api_time_set();
    ASSERT_EQ((uint16_t)(API_A | (API_X << 8)), (uint16_t)0xFFFF);
    ASSERT_EQ((int)API_ERRNO, (int)api_platform_errno(API_EACCES));

    /* And the clock it refused to move is still the host's. */
    xstack_ptr = XSTACK_SIZE;
    clk_api_time_get();
    ASSERT_EQ((uint16_t)(API_A | (API_X << 8)), (uint16_t)0);
    int64_t got;
    memcpy(&got, &xstack[xstack_ptr], 8);
    ASSERT_TRUE(got >= before);
    ASSERT_TRUE(got <= (int64_t)time(NULL));
}

UTEST_STATE();
int main(int argc, const char *const argv[])
{
    host_setenv("LC_ALL", "C"); /* deterministic strftime, adopted by the one sys_init */
    sys_init();              /* the drivers initialize exactly once */
    return utest_main(argc, argv);
}
