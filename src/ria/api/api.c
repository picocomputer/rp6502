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
                api_return_errno(API_ENOSYS);
    }
}

void api_run()
{
    // All registers reset to a known state
    for (int i = 0; i < 16; i++)
        if (i != 3) // Skip VSYNC
            REGS(i) = 0;
    API_STEP0 = 1;
    API_RW0 = xram[API_ADDR0];
    API_STEP1 = 1;
    API_RW1 = xram[API_ADDR1];
    api_return_errno(0);
}

bool api_pop_uint8_end(uint8_t *data)
{
    switch (xstack_ptr)
    {
    case XSTACK_SIZE - 0:
        *data = 0;
        return true;
    case XSTACK_SIZE - 1:
        memcpy((void *)data, &xstack[xstack_ptr], sizeof(uint8_t));
        api_zxstack();
        return true;
    default:
        return false;
    }
}

bool api_pop_uint16_end(uint16_t *data)
{
    switch (xstack_ptr)
    {
    case XSTACK_SIZE - 0:
        *data = 0;
        return true;
    case XSTACK_SIZE - 1:
        memcpy((void *)data + 1, &xstack[xstack_ptr], sizeof(uint16_t) - 1);
        *data >>= 8 * 1;
        api_zxstack();
        return true;
    case XSTACK_SIZE - 2:
        memcpy((void *)data + 0, &xstack[xstack_ptr], sizeof(uint16_t) - 0);
        *data >>= 8 * 0;
        api_zxstack();
        return true;
    default:
        return false;
    }
}

bool api_pop_uint32_end(uint32_t *data)
{
    switch (xstack_ptr)
    {
    case XSTACK_SIZE - 0:
        *data = 0;
        return true;
    case XSTACK_SIZE - 1:
        memcpy((void *)data + 3, &xstack[xstack_ptr], sizeof(uint32_t) - 3);
        *data >>= 8 * 3;
        api_zxstack();
        return true;
    case XSTACK_SIZE - 2:
        memcpy((void *)data + 2, &xstack[xstack_ptr], sizeof(uint32_t) - 2);
        *data >>= 8 * 2;
        api_zxstack();
        return true;
    case XSTACK_SIZE - 3:
        memcpy((void *)data + 1, &xstack[xstack_ptr], sizeof(uint32_t) - 1);
        *data >>= 8 * 1;
        api_zxstack();
        return true;
    case XSTACK_SIZE - 4:
        memcpy((void *)data + 0, &xstack[xstack_ptr], sizeof(uint32_t) - 0);
        *data >>= 8 * 0;
        api_zxstack();
        return true;
    default:
        return false;
    }
}

bool api_pop_int8_end(int8_t *data)
{
    switch (xstack_ptr)
    {
    case XSTACK_SIZE - 0:
        *data = 0;
        return true;
    case XSTACK_SIZE - 1:
        memcpy((void *)data, &xstack[xstack_ptr], sizeof(int8_t));
        api_zxstack();
        return true;
    default:
        return false;
    }
}

bool api_pop_int16_end(int16_t *data)
{
    switch (xstack_ptr)
    {
    case XSTACK_SIZE - 0:
        *data = 0;
        return true;
    case XSTACK_SIZE - 1:
        memcpy((void *)data + 1, &xstack[xstack_ptr], sizeof(int16_t) - 1);
        *data >>= 8 * 1;
        api_zxstack();
        return true;
    case XSTACK_SIZE - 2:
        memcpy((void *)data + 0, &xstack[xstack_ptr], sizeof(int16_t) - 0);
        *data >>= 8 * 0;
        api_zxstack();
        return true;
    default:
        return false;
    }
}

bool api_pop_int32_end(int32_t *data)
{
    switch (xstack_ptr)
    {
    case XSTACK_SIZE - 0:
        *data = 0;
        return true;
    case XSTACK_SIZE - 1:
        memcpy((void *)data + 3, &xstack[xstack_ptr], sizeof(int32_t) - 3);
        *data >>= 8 * 3;
        api_zxstack();
        return true;
    case XSTACK_SIZE - 2:
        memcpy((void *)data + 2, &xstack[xstack_ptr], sizeof(int32_t) - 2);
        *data >>= 8 * 2;
        api_zxstack();
        return true;
    case XSTACK_SIZE - 3:
        memcpy((void *)data + 1, &xstack[xstack_ptr], sizeof(int32_t) - 1);
        *data >>= 8 * 1;
        api_zxstack();
        return true;
    case XSTACK_SIZE - 4:
        memcpy((void *)data + 0, &xstack[xstack_ptr], sizeof(int32_t) - 0);
        *data >>= 8 * 0;
        api_zxstack();
        return true;
    default:
        return false;
    }
}
