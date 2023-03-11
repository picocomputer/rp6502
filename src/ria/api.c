/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api.h"
#include "ria.h"
#include "fatfs/ff.h"
#include "mem/regs.h"
#include "mem/vram.h"
#include "mem/vstack.h"

#include "stdio.h"     //TEMP
#include "pico/time.h" //TEMP

#define FIL_MAX 16
FIL fil_pool[FIL_MAX];

static uint16_t api_short_stack_uint16()
{

    if (vstack_ptr == VSTACK_SIZE - 1)
    {
        uint16_t val = *(uint8_t *)&vstack[vstack_ptr];
        vstack_ptr += 1;
        return val;
    }
    if (vstack_ptr == VSTACK_SIZE - 2)
    {
        uint16_t val = *(uint16_t *)&vstack[vstack_ptr];
        vstack_ptr += 2;
        return val;
    }
    return 0;
}

static uint32_t api_short_stack_uint32()
{
    if (vstack_ptr == VSTACK_SIZE - 3)
    {
        // TODO I think this faults
        uint32_t val = *(uint32_t *)&vstack[vstack_ptr] >> 8;
        vstack_ptr += 3;
        return val;
    }
    if (vstack_ptr == VSTACK_SIZE - 4)
    {
        uint32_t val = *(uint32_t *)&vstack[vstack_ptr];
        vstack_ptr += 4;
        return val;
    }
    return api_short_stack_uint16();
}

static uint64_t api_short_stack_uint64()
{

    if (vstack_ptr == VSTACK_SIZE - 8)
    {
        uint64_t val = *(uint64_t *)&vstack[vstack_ptr];
        vstack_ptr += 8;
        return val;
    }
    return api_short_stack_uint32();
}

static void api_test()
{
}

static void api_open(uint8_t *path)
{
    uint8_t mode = API_A;
    int i;
    vstack_ptr = VSTACK_SIZE;
    for (i = 0; i < FIL_MAX; i++)
        if (!fil_pool[i].obj.fs)
            break;
    if (i == FIL_MAX)
        // This error is "Number of open files > FF_FS_LOCK"
        // TODO Candidate for a new error
        return api_return_errno_ax(FR_TOO_MANY_OPEN_FILES, -1);
    FIL *fp = &fil_pool[i];
    FRESULT fresult = f_open(fp, (TCHAR *)path, mode);
    printf("(%d %s)\n", fresult, path);
    return api_return_errno_ax(fresult, i);
}

static void api_lseek()
{
    unsigned fd = API_AX;
    size_t ofs_ptr = vstack_ptr;
    vstack_ptr = VSTACK_SIZE;
    if (fd >= FIL_MAX || ofs_ptr != VSTACK_SIZE - 4)
        return api_return_errno_ax(FR_INVALID_PARAMETER, -1);
    uint32_t ofs = *(uint32_t *)&vstack[ofs_ptr];
    FIL *fp = &fil_pool[fd];
    FRESULT fresult = f_lseek(fp, ofs);
    FSIZE_t pos = f_tell(fp);
    // TODO additional checks?
    return api_return_errno_ax(fresult, pos);
}

static void api_read(uint8_t *buf)
{
    unsigned fd = API_AX;
    size_t count_ptr = vstack_ptr;
    vstack_ptr = VSTACK_SIZE;
    if (fd >= FIL_MAX || count_ptr != VSTACK_SIZE - 2)
        return api_return_errno_ax(FR_INVALID_PARAMETER, -1);
    uint16_t count = *(uint16_t *)&vstack[count_ptr];
    FIL *fp = &fil_pool[fd];
    UINT br;
    FRESULT fresult = f_read(fp, buf, count, &br);
    api_sync_vram();
    return api_return_errno_ax(fresult, br);
}

static void api_write(uint8_t *buf)
{
    unsigned fd = API_AX;
    size_t count_ptr = vstack_ptr;
    vstack_ptr = VSTACK_SIZE;
    if (fd >= FIL_MAX || count_ptr != VSTACK_SIZE - 2)
        return api_return_errno_ax(FR_INVALID_PARAMETER, -1);
    uint16_t count = *(uint16_t *)&vstack[count_ptr];
    FIL *fp = &fil_pool[fd];
    UINT bw;
    FRESULT fresult = f_write(fp, buf, count, &bw);
    return api_return_errno_ax(fresult, bw);
}

static void api_close()
{
    unsigned fd = API_AX;
    FIL *fp = &fil_pool[fd];
    FRESULT fresult = f_close(fp);
    return api_return_errno_ax(fresult, fresult == FR_OK ? 0 : -1);
}

static void api_set_vreg()
{
    unsigned regno = API_A;
    uint16_t data = api_short_stack_uint16();
    if (vstack_ptr != VSTACK_SIZE)
        return api_return_errno_ax(FR_INVALID_PARAMETER, -1);
    RIA_PIX_PIO->txf[RIA_PIX_SM] = (regno << 16) | data | RIA_PIX_REGS;
    return api_return_ax(0);
}

void api_task()
{
    if (API_BUSY)
        switch (API_OP) // 1-127 valid
        {
        case 0x00:
        case 0xFF:
            // action loop handles these
            break;
        case 0x01:
            api_open(&vstack[vstack_ptr]);
            break;
        case 0x02:
            api_open(&vram[vram_ptr0]);
            break;
        case 0x03:
            api_open(&vram[vram_ptr1]);
            break;
        case 0x04:
            api_close();
            break;
        case 0x05:
            api_read(&vram[vram_ptr0]);
            break;
        case 0x06:
            api_read(&vram[vram_ptr1]);
            break;
        case 0x07:
            api_write(&vram[vram_ptr0]);
            break;
        case 0x08:
            api_write(&vram[vram_ptr1]);
            break;
        case 0x09:
            api_lseek();
            break;
        case 0x10:
            api_set_vreg();
            break;
        default:
            // TODO report an error
            //  API_ERRNO = EUNKNOWN;
            api_return_released();
            break;
        }
}

void api_stop()
{
    RIA_PIX_PIO->txf[RIA_PIX_SM] = 0 | 0 | RIA_PIX_REGS;
    for (int i = 0; i < FIL_MAX; i++)
        if (fil_pool[i].obj.fs)
            f_close(&fil_pool[i]);
}

void api_reset()
{
    for (int i = 0; i < 16; i++)
        REGS(i) = 0;
    vstack_ptr = VSTACK_SIZE;
}
