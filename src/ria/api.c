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
    API_RETURN_VAL_ERR(br, fresult);
}

void api_task()
{
    switch (API_OPCODE) // 1-127 valid
    {
    case 1:
        api_open(&vram[vram_ptr0]);
        break;
    case 2:
        api_lseek();
        break;
    case 3:
        api_read(&vram[vram_ptr0]);
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
