/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api.h"
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
    uint8_t mode = API_AX;
    int i;
    for (i = 0; i < FIL_MAX; i++)
        if (!fil_pool[i].obj.fs)
            break;
    if (i == FIL_MAX)
    {
        // This error is "Number of open files > FF_FS_LOCK"
        // TODO Candidate for a new error
        API_RETURN_VAL_ERR(-1, FR_TOO_MANY_OPEN_FILES);
        return;
    }
    FIL *fp = &fil_pool[i];
    FRESULT fresult = f_open(fp, (TCHAR *)path, mode);
    API_RETURN_VAL_ERR(i, fresult);
}

static void api_lseek()
{
    unsigned fd = API_AX;
    if (fd >= FIL_MAX || vstack_ptr != VSTACK_SIZE - 4)
    {
        API_RETURN_VAL_ERR(-1, FR_INVALID_PARAMETER);
        return;
    }
    uint32_t ofs = *(uint32_t *)&vstack[vstack_ptr];
    FIL *fp = &fil_pool[fd];
    FRESULT fresult = f_lseek(fp, ofs);
    FSIZE_t pos = f_tell(fp);
    // TODO additional checks?
    API_RETURN_VAL_ERR(pos, fresult);
}

static void api_read(uint8_t *buf)
{
    unsigned fd = API_AX;
    if (fd >= FIL_MAX || vstack_ptr != VSTACK_SIZE - 2)
    {
        API_RETURN_VAL_ERR(-1, FR_INVALID_PARAMETER);
        return;
    }
    uint16_t count = *(uint16_t *)&vstack[vstack_ptr];
    FIL *fp = &fil_pool[fd];
    UINT br;
    FRESULT fresult = f_read(fp, buf, count, &br);
    API_RETURN_VRAM();
    API_RETURN_VAL_ERR(br, fresult);
}

static void api_write(uint8_t *buf)
{
    unsigned fd = API_AX;
    if (fd >= FIL_MAX || vstack_ptr != VSTACK_SIZE - 2)
    {
        API_RETURN_VAL_ERR(-1, FR_INVALID_PARAMETER)
        return;
    }
    uint16_t count = *(uint16_t *)&vstack[vstack_ptr];
    FIL *fp = &fil_pool[fd];
    UINT bw;
    FRESULT fresult = f_write(fp, buf, count, &bw);
    API_RETURN_VAL_ERR(bw, fresult);
}

static void api_close()
{
    unsigned fd = API_AX;
    FIL *fp = &fil_pool[fd];
    FRESULT fresult = f_close(fp);
    if (fresult == FR_OK)
        API_RETURN_VAL_ERR(0, fresult)
    else
        API_RETURN_VAL_ERR(-1, fresult)
}

void api_task()
{
    switch (API_OPCODE) // 1-127 valid
    {
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
    }
}

void api_stop()
{
    for (int i = 0; i < FIL_MAX; i++)
        if (fil_pool[i].obj.fs)
            f_close(&fil_pool[i]);
}

void api_reset()
{
    API_OPCODE = 0xFF;
    vstack_ptr = VSTACK_SIZE;
}
