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
FIL fp[FIL_MAX];

static void api_open(uint8_t *path, uint8_t mode)
{
    int i;
    for (i = 0; i < FIL_MAX; i++)
        if (!fp[i].obj.fs)
            break;
    if (i == FIL_MAX)
    {
        // This error is "Number of open files > FF_FS_LOCK"
        //TODO Candidate for a new error
        API_RETURN_VAL_ERR(-1, FR_TOO_MANY_OPEN_FILES);
        return;
    }
    FRESULT fresult = f_open(&fp[i], (TCHAR *)path, mode);
    // printf("<open0> mode:%d path:%s\n", mode, path);
    // printf("<open0> val:%d err:%d\n", i, fresult);
    API_RETURN_VAL_ERR(i, fresult);
}

void api_task()
{
    switch (API_OPCODE) // 1-127 valid
    {
    case 1: // open0
        api_open(&vram[vram_ptr0], API_AX);
        break;
    }
}

void api_stop()
{
    for (int i = 0; i < FIL_MAX; i++)
        if (fp[i].obj.fs)
            f_close(&fp[i]);
}

void api_reset()
{
    API_OPCODE = 0xFF;
    vstack_ptr = VSTACK_SIZE;
}
