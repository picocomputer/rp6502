/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "api/api.h"
#include "sys/cpu.h"
#include "sys/ria.h"

void api_task(void)
{
    static uint8_t active_op = 0;
    // Latch called op in case 6502 app misbehaves
    if (cpu_active() && !ria_active() &&
        !active_op && API_BUSY &&
        API_OP != 0x00 && API_OP != 0xFF)
        active_op = API_OP;
    if (active_op)
    {
        if (!main_api(active_op))
            api_return_errno(API_ENOSYS);
        if (!API_BUSY)
            active_op = 0;
    }
}

void api_run(void)
{
    // All registers reset to a known state
    for (int i = 0; i < 16; i++)
        if (i != 3) // Skip VSYNC
            REGS(i) = 0;
    *(int8_t *)&REGS(0xFFE5) = 1; // STEP0
    REGS(0xFFE4) = xram[0];       // RW0
    *(int8_t *)&REGS(0xFFE9) = 1; // STEP1
    REGS(0xFFE8) = xram[0];       // RW1
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
