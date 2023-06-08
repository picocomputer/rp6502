/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "api/api.h"
#include "sys/cpu.h"
#include "sys/ria.h"
#include "fatfs/ff.h"

#ifdef NDEBUG
uint8_t xram[0x10000];
#else
static struct
{
    uint8_t _0[0x1000];
    uint8_t _1[0x1000];
    uint8_t _2[0x1000];
    uint8_t _3[0x1000];
    uint8_t _4[0x1000];
    uint8_t _5[0x1000];
    uint8_t _6[0x1000];
    uint8_t _7[0x1000];
    uint8_t _8[0x1000];
    uint8_t _9[0x1000];
    uint8_t _A[0x1000];
    uint8_t _B[0x1000];
    uint8_t _C[0x1000];
    uint8_t _D[0x1000];
    uint8_t _E[0x1000];
    uint8_t _F[0x1000];
    // this struct of 4KB segments is because
    // a single 64KB array crashes my debugger
} xram_blocks;
uint8_t *const xram = (uint8_t *)&xram_blocks;
#endif

uint8_t xstack[XSTACK_SIZE + 1];
size_t volatile xstack_ptr;

void api_task()
{
    if (cpu_active() && !ria_active() && API_BUSY)
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
    API_STEP0 = 1;
    API_ADDR0 = 0;
    API_RW0 = xram[API_ADDR0];
    API_STEP1 = 1;
    API_ADDR1 = 0;
    API_RW1 = xram[API_ADDR1];
    xstack_ptr = XSTACK_SIZE;
    api_return_errno_axsreg_zxstack(0, 0);
}

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
