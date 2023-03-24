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

#define FIL_MAX 16
FIL fil_pool[FIL_MAX];
// 0,1,2 reserved for STDIN, STDOUT, STDERR
#define FIL_OFFS 3
static_assert(FIL_MAX + FIL_OFFS < 128);

uint16_t api_sstack_uint16()
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

uint32_t api_sstack_uint32()
{
    if (vstack_ptr == VSTACK_SIZE - 3)
    {
        uint32_t val = *(uint32_t *)&vstack[vstack_ptr - 1] >> 8;
        vstack_ptr += 3;
        return val;
    }
    if (vstack_ptr == VSTACK_SIZE - 4)
    {
        uint32_t val = *(uint32_t *)&vstack[vstack_ptr];
        vstack_ptr += 4;
        return val;
    }
    return api_sstack_uint16();
}

uint64_t api_sstack_uint64()
{
    if (vstack_ptr == VSTACK_SIZE - 5)
    {
        uint64_t val = *(uint64_t *)&vstack[vstack_ptr - 3] >> 24;
        vstack_ptr += 5;
        return val;
    }
    if (vstack_ptr == VSTACK_SIZE - 6)
    {
        uint64_t val = *(uint64_t *)&vstack[vstack_ptr - 2] >> 16;
        vstack_ptr += 6;
        return val;
    }
    if (vstack_ptr == VSTACK_SIZE - 7)
    {
        uint64_t val = *(uint64_t *)&vstack[vstack_ptr - 1] >> 8;
        vstack_ptr += 7;
        return val;
    }
    if (vstack_ptr == VSTACK_SIZE - 8)
    {
        uint64_t val = *(uint64_t *)&vstack[vstack_ptr];
        vstack_ptr += 8;
        return val;
    }
    return api_sstack_uint32();
}

int16_t api_sstack_int16()
{

    if (vstack_ptr == VSTACK_SIZE - 1)
    {
        int16_t val = *(int8_t *)&vstack[vstack_ptr];
        vstack_ptr += 1;
        return val;
    }
    if (vstack_ptr == VSTACK_SIZE - 2)
    {
        int16_t val = *(int16_t *)&vstack[vstack_ptr];
        vstack_ptr += 2;
        return val;
    }
    return 0;
}

int32_t api_sstack_int32()
{
    if (vstack_ptr == VSTACK_SIZE - 3)
    {
        int32_t val = *(int32_t *)&vstack[vstack_ptr - 1] >> 8;
        vstack_ptr += 3;
        return val;
    }
    if (vstack_ptr == VSTACK_SIZE - 4)
    {
        int32_t val = *(int32_t *)&vstack[vstack_ptr];
        vstack_ptr += 4;
        return val;
    }
    return api_sstack_int16();
}

int64_t api_sstack_int64()
{
    if (vstack_ptr == VSTACK_SIZE - 5)
    {
        int64_t val = *(int64_t *)&vstack[vstack_ptr - 3] >> 24;
        vstack_ptr += 5;
        return val;
    }
    if (vstack_ptr == VSTACK_SIZE - 6)
    {
        int64_t val = *(int64_t *)&vstack[vstack_ptr - 2] >> 16;
        vstack_ptr += 6;
        return val;
    }
    if (vstack_ptr == VSTACK_SIZE - 7)
    {
        int64_t val = *(int64_t *)&vstack[vstack_ptr - 1] >> 8;
        vstack_ptr += 7;
        return val;
    }
    if (vstack_ptr == VSTACK_SIZE - 8)
    {
        int64_t val = *(int64_t *)&vstack[vstack_ptr];
        vstack_ptr += 8;
        return val;
    }
    return api_sstack_int32();
}

static void api_open(uint8_t *path)
{
    // These match CC65 which is closer to POSIX than FatFs.
    const unsigned char RDWR = 0x03;
    const unsigned char CREAT = 0x10;
    const unsigned char TRUNC = 0x20;
    const unsigned char APPEND = 0x40;
    const unsigned char EXCL = 0x80;

    uint8_t flags = API_A;
    uint8_t mode = flags & RDWR; // RDWR are same bits
    assert((FA_READ | FA_WRITE) == RDWR);

    if (flags & CREAT)
    {
        if (flags & EXCL)
            mode |= FA_CREATE_NEW;
        else
        {
            if (flags & TRUNC)
                mode |= FA_CREATE_ALWAYS;
            else if (flags & APPEND)
                mode |= FA_OPEN_APPEND;
            else
                mode |= FA_OPEN_ALWAYS;
        }
    }

    int i;
    vstack_ptr = VSTACK_SIZE;
    for (i = 0; i < FIL_MAX; i++)
        if (!fil_pool[i].obj.fs)
            break;
    if (i == FIL_MAX)
        return api_return_errno_ax(FR_TOO_MANY_OPEN_FILES, -1);
    FIL *fp = &fil_pool[i];
    FRESULT fresult = f_open(fp, (TCHAR *)path, mode);
    if (fresult != FR_OK)
        return api_return_errno_axsreg(fresult, -1);
    return api_return_ax(i + FIL_OFFS);
}

static void api_lseek()
{
    unsigned fd = API_A;
    uint64_t ofs = api_sstack_uint64();
    if (vstack_ptr != VSTACK_SIZE || fd < FIL_OFFS || fd >= FIL_MAX + FIL_OFFS)
        return api_return_errno_ax(FR_INVALID_PARAMETER, -1);
    FIL *fp = &fil_pool[fd - FIL_OFFS];
    FRESULT fresult = f_lseek(fp, ofs);
    if (fresult != FR_OK)
        return api_return_errno_axsreg(fresult, -1);
    FSIZE_t pos = f_tell(fp);
    if (pos > 0x0FFFFFFF)
        pos = 0x0FFFFFFF;
    return api_return_axsreg(pos);
}

static void api_read(uint8_t *buf)
{
    unsigned fd = API_AX;
    size_t count_ptr = vstack_ptr;
    vstack_ptr = VSTACK_SIZE;
    if (fd >= FIL_MAX + FIL_OFFS || count_ptr != VSTACK_SIZE - 2)
        return api_return_errno_ax(FR_INVALID_PARAMETER, -1);
    uint16_t count = *(uint16_t *)&vstack[count_ptr];
    FIL *fp = &fil_pool[fd - FIL_OFFS];
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
    if (fd >= FIL_MAX + FIL_OFFS || count_ptr != VSTACK_SIZE - 2)
        return api_return_errno_ax(FR_INVALID_PARAMETER, -1);
    uint16_t count = *(uint16_t *)&vstack[count_ptr];
    FIL *fp = &fil_pool[fd - FIL_OFFS];
    UINT bw;
    FRESULT fresult = f_write(fp, buf, count, &bw);
    return api_return_errno_ax(fresult, bw);
}

static void api_close()
{
    unsigned fd = API_AX;
    FIL *fp = &fil_pool[fd - FIL_OFFS];
    FRESULT fresult = f_close(fp);
    return api_return_errno_ax(fresult, fresult == FR_OK ? 0 : -1);
}

static void api_set_vreg()
{
    unsigned regno = API_A;
    uint16_t data = api_sstack_uint16();
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
