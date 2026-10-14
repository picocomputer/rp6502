/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/config.h"
#include "core/sys/sys.h"
#include "core/str/oem.h"
#include "emu_boot.h"

UTEST(rtc, code_page_drives_oem_mapping)
{
    oem_set_code_page_run(437);
    ASSERT_EQ(oem_get_code_page_run(), (uint16_t)437);

    char out[8];
    oem_from_utf8("\xC3\xA3", out, sizeof out); /* "ã" UTF-8 */
    ASSERT_EQ((unsigned char)out[0], 0x7F);      /* not in CP437 */

    ASSERT_FALSE(oem_set_code_page(999));
    ASSERT_EQ(oem_get_code_page_run(), (uint16_t)437);

    oem_set_code_page_run(850);
    ASSERT_EQ(oem_get_code_page_run(), (uint16_t)850);
    oem_from_utf8("\xC3\xA3", out, sizeof out);
    ASSERT_EQ((unsigned char)out[0], 0xC6); /* CP850 'ã' */
}

UTEST_STATE();
int main(int argc, const char *const argv[])
{
    sys_init();
    return utest_main(argc, argv);
}
