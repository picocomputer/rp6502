/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/api/api.h"
#include "core/api/attr.h"
#include "core/ria/regs.h"
#include "utest.h"
#include <string.h>

#define ATTR_PHI2_KHZ 0x01
#define ATTR_CODE_PAGE 0x02

static int16_t attr_set(uint8_t id, uint32_t value)
{
    xstack_ptr = XSTACK_SIZE - sizeof value;
    memcpy(&xstack[xstack_ptr], &value, sizeof value);
    API_A = id;
    attr_api_set();
    return (int16_t)API_AX;
}

static uint16_t attr_get(uint8_t id)
{
    API_A = id;
    attr_api_get();
    return API_AX;
}

UTEST(attr, a_rate_outside_100_to_8000_fails_with_einval)
{
    api_set_errno_opt(2);
    static const uint32_t refused[] = {0, 99, 8001, 65535};
    for (size_t i = 0; i < sizeof refused / sizeof *refused; i++)
    {
        ASSERT_EQ(attr_set(ATTR_PHI2_KHZ, refused[i]), -1);
        ASSERT_EQ(API_ERRNO, api_platform_errno(API_EINVAL));
    }
    static const uint32_t accepted[] = {100, 8000};
    for (size_t i = 0; i < sizeof accepted / sizeof *accepted; i++)
    {
        ASSERT_EQ(attr_set(ATTR_PHI2_KHZ, accepted[i]), 0);
        ASSERT_EQ(attr_get(ATTR_PHI2_KHZ), accepted[i]);
    }
}

UTEST(attr, a_value_that_is_not_a_code_page_fails_with_einval)
{
    api_set_errno_opt(2);
    ASSERT_EQ(attr_set(ATTR_CODE_PAGE, 850), 0);
    static const uint32_t refused[] = {0, 438, 60000, 65536};
    for (size_t i = 0; i < sizeof refused / sizeof *refused; i++)
    {
        ASSERT_EQ(attr_set(ATTR_CODE_PAGE, refused[i]), -1);
        ASSERT_EQ(API_ERRNO, api_platform_errno(API_EINVAL));
        ASSERT_EQ(attr_get(ATTR_CODE_PAGE), 850);
    }
    ASSERT_EQ(attr_set(ATTR_CODE_PAGE, 437), 0);
    ASSERT_EQ(attr_get(ATTR_CODE_PAGE), 437);
}

UTEST_MAIN();
