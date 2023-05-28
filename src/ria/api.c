/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api.h"
#include "cpu.h"
#include "main.h"
#include "fatfs/ff.h"

uint16_t api_sstack_uint16()
{

    if (xstack_ptr == XSTACK_SIZE - 1)
    {
        uint16_t val = *(uint8_t *)&xstack[xstack_ptr];
        xstack_ptr += 1;
        return val;
    }
    if (xstack_ptr == XSTACK_SIZE - 2)
    {
        uint16_t val = *(uint16_t *)&xstack[xstack_ptr];
        xstack_ptr += 2;
        return val;
    }
    return 0;
}

uint32_t api_sstack_uint32()
{
    if (xstack_ptr == XSTACK_SIZE - 3)
    {
        uint32_t val = *(uint32_t *)&xstack[xstack_ptr - 1] >> 8;
        xstack_ptr += 3;
        return val;
    }
    if (xstack_ptr == XSTACK_SIZE - 4)
    {
        uint32_t val = *(uint32_t *)&xstack[xstack_ptr];
        xstack_ptr += 4;
        return val;
    }
    return api_sstack_uint16();
}

uint64_t api_sstack_uint64()
{
    if (xstack_ptr == XSTACK_SIZE - 5)
    {
        uint64_t val = *(uint64_t *)&xstack[xstack_ptr - 3] >> 24;
        xstack_ptr += 5;
        return val;
    }
    if (xstack_ptr == XSTACK_SIZE - 6)
    {
        uint64_t val = *(uint64_t *)&xstack[xstack_ptr - 2] >> 16;
        xstack_ptr += 6;
        return val;
    }
    if (xstack_ptr == XSTACK_SIZE - 7)
    {
        uint64_t val = *(uint64_t *)&xstack[xstack_ptr - 1] >> 8;
        xstack_ptr += 7;
        return val;
    }
    if (xstack_ptr == XSTACK_SIZE - 8)
    {
        uint64_t val = *(uint64_t *)&xstack[xstack_ptr];
        xstack_ptr += 8;
        return val;
    }
    return api_sstack_uint32();
}

int16_t api_sstack_int16()
{

    if (xstack_ptr == XSTACK_SIZE - 1)
    {
        int16_t val = *(int8_t *)&xstack[xstack_ptr];
        xstack_ptr += 1;
        return val;
    }
    if (xstack_ptr == XSTACK_SIZE - 2)
    {
        int16_t val = *(int16_t *)&xstack[xstack_ptr];
        xstack_ptr += 2;
        return val;
    }
    return 0;
}

int32_t api_sstack_int32()
{
    if (xstack_ptr == XSTACK_SIZE - 3)
    {
        int32_t val = *(int32_t *)&xstack[xstack_ptr - 1] >> 8;
        xstack_ptr += 3;
        return val;
    }
    if (xstack_ptr == XSTACK_SIZE - 4)
    {
        int32_t val = *(int32_t *)&xstack[xstack_ptr];
        xstack_ptr += 4;
        return val;
    }
    return api_sstack_int16();
}

int64_t api_sstack_int64()
{
    if (xstack_ptr == XSTACK_SIZE - 5)
    {
        int64_t val = *(int64_t *)&xstack[xstack_ptr - 3] >> 24;
        xstack_ptr += 5;
        return val;
    }
    if (xstack_ptr == XSTACK_SIZE - 6)
    {
        int64_t val = *(int64_t *)&xstack[xstack_ptr - 2] >> 16;
        xstack_ptr += 6;
        return val;
    }
    if (xstack_ptr == XSTACK_SIZE - 7)
    {
        int64_t val = *(int64_t *)&xstack[xstack_ptr - 1] >> 8;
        xstack_ptr += 7;
        return val;
    }
    if (xstack_ptr == XSTACK_SIZE - 8)
    {
        int64_t val = *(int64_t *)&xstack[xstack_ptr];
        xstack_ptr += 8;
        return val;
    }
    return api_sstack_int32();
}

void api_task()
{
    if (cpu_is_running() && API_BUSY)
    {
        uint8_t operation = API_OP;
        if (operation != 0x00 && operation != 0xFF)
            if (!main_api(operation))
            {
                API_ERRNO = FR_INVALID_PARAMETER; // EUNKNOWN
                api_return_released();
            }
    }
}

void api_run()
{
    // All registers reset to a known state
    for (int i = 0; i < 16; i++)
        REGS(i) = 0;
    XRAM_STEP0 = 1;
    XRAM_STEP1 = 1;
    xstack_ptr = XSTACK_SIZE;
    api_return_errno_axsreg_zxstack(0, 0);
}
