/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/api/api.h"
#include "core/api/clk.h"
#include "core/ria/regs.h"
#include "utest.h"
#include <string.h>

static void push_time(int64_t sec)
{
    xstack_ptr = XSTACK_SIZE - sizeof sec;
    memcpy(&xstack[xstack_ptr], &sec, sizeof sec);
}

/* 2025-01-01 converts. Year 40000 does not fit the 16-bit tm_year, and
 * INT64_MAX does not fit the host's own year. */
UTEST(clk, a_time_that_does_not_convert_fails_with_erange)
{
    api_set_errno_opt(2);
    bool (*const calls[])(void) = {clk_api_gmtime, clk_api_localtime};
    for (size_t i = 0; i < sizeof calls / sizeof *calls; i++)
    {
        push_time(1735689600);
        calls[i]();
        ASSERT_EQ((int16_t)API_AX, 0);
        xstack_ptr = XSTACK_SIZE;
        static const int64_t refused[] = {1199000000000LL, INT64_MAX};
        for (size_t j = 0; j < sizeof refused / sizeof *refused; j++)
        {
            push_time(refused[j]);
            calls[i]();
            ASSERT_EQ((int16_t)API_AX, -1);
            ASSERT_EQ(API_ERRNO, api_platform_errno(API_ERANGE));
        }
    }
}

UTEST_MAIN();
