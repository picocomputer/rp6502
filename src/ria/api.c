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

static void api_open(uint8_t *path)
{
    uint8_t mode = API_A;
    int i;
    vstack_ptr = VSTACK_SIZE;
    for (i = 0; i < FIL_MAX; i++)
        if (!fil_pool[i].obj.fs)
            break;
    if (i == FIL_MAX)
    {
        // This error is "Number of open files > FF_FS_LOCK"
        // TODO Candidate for a new error
        API_ERRNO = FR_TOO_MANY_OPEN_FILES;
        API_RETURN_AX(-1);
        return;
    }
    FIL *fp = &fil_pool[i];
    FRESULT fresult = f_open(fp, (TCHAR *)path, mode);
    API_ERRNO = fresult;
    API_RETURN_AX(i);
}

static void api_lseek()
{
    unsigned fd = API_AX;
    size_t ofs_ptr = vstack_ptr;
    vstack_ptr = VSTACK_SIZE;
    if (fd >= FIL_MAX || ofs_ptr != VSTACK_SIZE - 4)
    {
        API_ERRNO = FR_INVALID_PARAMETER;
        API_RETURN_AX(-1);
        return;
    }
    uint32_t ofs = *(uint32_t *)&vstack[ofs_ptr];
    FIL *fp = &fil_pool[fd];
    FRESULT fresult = f_lseek(fp, ofs);
    FSIZE_t pos = f_tell(fp);
    // TODO additional checks?
    API_ERRNO = fresult;
    API_RETURN_AX(pos);
}

static void api_read(uint8_t *buf)
{
    unsigned fd = API_AX;
    size_t count_ptr = vstack_ptr;
    vstack_ptr = VSTACK_SIZE;
    if (fd >= FIL_MAX || count_ptr != VSTACK_SIZE - 2)
    {
        API_ERRNO = FR_INVALID_PARAMETER;
        API_RETURN_AX(-1);
        return;
    }
    uint16_t count = *(uint16_t *)&vstack[count_ptr];
    FIL *fp = &fil_pool[fd];
    UINT br;
    FRESULT fresult = f_read(fp, buf, count, &br);
    API_RETURN_VRAM();
    API_ERRNO = fresult;
    API_RETURN_AX(br);
}

static void api_write(uint8_t *buf)
{
    unsigned fd = API_AX;
    size_t count_ptr = vstack_ptr;
    vstack_ptr = VSTACK_SIZE;
    if (fd >= FIL_MAX || count_ptr != VSTACK_SIZE - 2)
    {
        API_ERRNO = FR_INVALID_PARAMETER;
        API_RETURN_AX(-1);
        return;
    }
    uint16_t count = *(uint16_t *)&vstack[count_ptr];
    FIL *fp = &fil_pool[fd];
    UINT bw;
    FRESULT fresult = f_write(fp, buf, count, &bw);
    API_ERRNO = fresult;
    API_RETURN_AX(bw);
}

static void api_close()
{
    unsigned fd = API_AX;
    FIL *fp = &fil_pool[fd];
    FRESULT fresult = f_close(fp);
    API_ERRNO = fresult;
    if (fresult == FR_OK)
        API_RETURN_AX(0)
    else
        API_RETURN_AX(-1)
}

static void api_set_vreg()
{
    unsigned regno = API_A;
    size_t data_ptr = vstack_ptr;
    vstack_ptr = VSTACK_SIZE;
    if (data_ptr != VSTACK_SIZE - 2)
    {
        API_ERRNO = FR_INVALID_PARAMETER;
        API_RETURN_AX(-1);
        return;
    }
    uint16_t data = *(uint16_t *)&vstack[data_ptr];
    RIA_PIX_PIO->txf[RIA_PIX_SM] = (regno << 16) | data | RIA_PIX_REGS;
    API_RETURN_AX(0);
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
            API_SPIN_RELEASE();
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
