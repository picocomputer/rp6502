/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/config.h"
#include "core/api/api.h"
#include "core/api/clk.h"
#include "core/sys/sys.h"
#include "core/str/oem.h"
#include "core/ria/regs.h"
#include "tb_hostos.h"
#include "emu_boot.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct wire_tm
{
    int16_t sec, min, hour, mday, mon, year, wday, yday, isdst;
};

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

UTEST(rtc, strftime_maps_utf8_to_oem)
{
    oem_set_code_page_run(437);

    struct wire_tm w = {0, 0, 12, 1, 0, 125, 3, 0, 0};
    char out[16];
    uint16_t n = drive_strftime(&w, "caf\xC3\xA9", out, sizeof out); /* "café" UTF-8 */
    ASSERT_EQ(n, (uint16_t)4);
    ASSERT_EQ((unsigned char)out[3], 0x82); /* CP437 'é' */
    ASSERT_EQ(out[0], 'c');
}

UTEST(rtc, strftime_z_uses_host_offset)
{
    host_setenv("TZ", "PST8");
    tzset();

    struct wire_tm w = {0, 0, 12, 1, 6, 125, 2, 181, 0}; /* 2025-07-01, no DST */
    char out[16];
    drive_strftime(&w, "%z", out, sizeof out);
    ASSERT_STREQ(out, "-0800");
}

UTEST(rtc, code_page_drives_oem_mapping)
{
    oem_set_code_page_run(437);
    ASSERT_EQ(oem_get_code_page_run(), (uint16_t)437);

    struct wire_tm w = {0, 0, 12, 1, 0, 125, 3, 0, 0};
    char out[8];
    drive_strftime(&w, "\xC3\xA3", out, sizeof out); /* "ã" UTF-8 */
    ASSERT_EQ((unsigned char)out[0], 0x7F);          /* not in CP437 */

    ASSERT_FALSE(oem_set_code_page(999));
    ASSERT_EQ(oem_get_code_page_run(), (uint16_t)437);

    oem_set_code_page_run(850);
    ASSERT_EQ(oem_get_code_page_run(), (uint16_t)850);
    drive_strftime(&w, "\xC3\xA3", out, sizeof out);
    ASSERT_EQ((unsigned char)out[0], 0xC6); /* CP850 'ã' */
}

UTEST(rtc, stop_reverts_run_code_page)
{
    /* An earlier case can leave a run code page set while no program is
     * running. The stop inside emu_restart calls the drivers' stop hooks only
     * when a program is running, so emu_restart alone does not reset the run
     * page, and this stop and commit reset it to the page that the config or
     * the locale selects. */
    ASSERT_TRUE(emu_restart(ROMS_DIR "/mode3_1bpp.rp6502"));
    sys_stop();
    sys_commit();
    const uint16_t resolved = oem_get_code_page_run();

    ASSERT_TRUE(emu_restart(ROMS_DIR "/mode3_1bpp.rp6502"));
    const uint16_t guest = resolved == 850 ? 437 : 850;
    oem_set_code_page_run(guest);
    ASSERT_EQ(oem_get_code_page_run(), guest);
    sys_stop();
    sys_commit();
    ASSERT_EQ(oem_get_code_page_run(), resolved);
}

UTEST(rtc, settime_is_refused_on_a_machine_that_does_not_own_the_clock)
{
    /* Option 2 is the llvm-mos errno map. A map is selected because every
     * errno is -1 until one is. */
    api_set_errno_opt(2);
    const int64_t before = (int64_t)time(NULL);
    const int64_t want = 1735732800; /* 2025-01-01 noon UTC */
    memcpy(&xstack[XSTACK_SIZE - 8], &want, 8);
    xstack_ptr = XSTACK_SIZE - 8;
    clk_api_time_set();
    ASSERT_EQ((uint16_t)(API_A | (API_X << 8)), (uint16_t)0xFFFF);
    ASSERT_EQ((int)API_ERRNO, (int)api_platform_errno(API_EACCES));

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
    host_setenv("LC_ALL", "C");
    sys_init();
    return utest_main(argc, argv);
}
