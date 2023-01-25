/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "fil.h"
#include "str.h"
#include "mon.h"
#include "ria/ria.h"
#include "mem/mbuf.h"
#include "fatfs/ff.h"
#include <stdio.h>
#include "pico/stdlib.h"

#define TIMEOUT_MS 200

static enum {
    FIL_IDLE,
    FIL_COMMAND,
    FIL_BINARY,
} fil_state;

// static uint32_t rw_addr;
static uint32_t rx_len;
static uint32_t rx_crc;
static absolute_time_t watchdog;
static FIL fil_fat;

void fil_chdir(const char *args, size_t len)
{
    FRESULT result;
    DIR dir;
    if (!len)
    {
        char s[256];
        result = f_getcwd(s, 256);
        if (result != FR_OK)
            printf("?Current working directory unknown (%d)\n", result);
        else
            printf("%s\n", s);
        return;
    }
    result = f_opendir(&dir, args);
    if (result != FR_OK)
        printf("?Directory not found (%d)\n", result);
    if (result == FR_OK)
    {
        result = f_closedir(&dir);
        if (result != FR_OK)
            printf("?Unable to close directory (%d)\n", result);
    }
    if (result == FR_OK)
    {
        result = f_chdir(args);
        if (result != FR_OK)
            printf("?Unable to change directory (%d)\n", result);
    }
}

void fil_chdrive(const char *args, size_t len)
{
    assert(len >= 2 && args[1] == ':');
    char s[3] = "0:";
    s[0] = args[0];
    FRESULT result;
    DIR dir;
    result = f_opendir(&dir, s);
    if (result != FR_OK)
        printf("?Drive not found (%d)\n", result);
    if (result == FR_OK)
    {
        result = f_closedir(&dir);
        if (result != FR_OK)
            printf("?Unable to close directory (%d)\n", result);
    }
    if (result == FR_OK)
    {
        result = f_chdrive(s);
        if (result != FR_OK)
            printf("?Unable to change drive (%d)\n", result);
    }
}

void fil_ls(const char *args, size_t len)
{
    // TODO make nice
    const char *dpath = ".";
    if (len)
        dpath = args;
    DIR dir;
    if (FR_OK != f_opendir(&dir, dpath))
    {
        printf("?cannot access '%s': No such directory.\n", dpath);
        return;
    }
    FILINFO fno;
    while ((f_readdir(&dir, &fno) == FR_OK) && (fno.fname[0] != 0))
    {
        if (fno.fname[0] != '.')
        {
            if (fno.fattrib & AM_DIR)
                printf("<DIR> %s\n", fno.fname);
            else
                printf("      %s\n", fno.fname);
        }
    }

    f_closedir(&dir);
}

void fil_upload(const char *args, size_t len)
{
    if (!len)
    {
        printf("?missing filename\n");
        return;
    }
    FRESULT result = f_open(&fil_fat, args, FA_READ | FA_WRITE);
    if (result == FR_NO_FILE)
        result = f_open(&fil_fat, args, FA_CREATE_NEW | FA_WRITE);
    if (result != FR_OK)
    {
        printf("?Unable to open file (%d)\n", result);
        return;
    }
    fil_state = FIL_COMMAND;
    fil_keep_alive();
}

void fil_command_dispatch(const char *args, size_t len)
{
    if (len == 0 || (len == 3 && !strnicmp("END", args, 3)))
    {
        fil_state = FIL_IDLE;
        FRESULT result = f_close(&fil_fat);
        if (result != FR_OK)
            printf("?Unable to close file (%d)\n", result);
        return;
    }

    if (parse_uint32(&args, &len, &rx_len) &&
        parse_uint32(&args, &len, &rx_crc) &&
        parse_end(args, len))
    {
        if (!rx_len || rx_len > MBUF_SIZE)
        {
            fil_state = FIL_IDLE;
            printf("?invalid length\n");
            return;
        }
        mbuf_len = 0;
        fil_state = FIL_BINARY;
        fil_keep_alive();
        return;
    }
    printf("?invalid argument\n");
    fil_state = FIL_IDLE;
    return;
}

bool fil_rx_handler()
{
    if (mbuf_len < rx_len)
        return false;
    FRESULT result = FR_OK;
    if (mbuf_crc32() != rx_crc)
    {
        result = FR_INT_ERR; // any error to abort
        puts("?CRC does not match");
    }
    // This will leave the file unchanged until
    // the first chunk is received successfully.
    if (result == FR_OK && f_tell(&fil_fat) == 0)
    {
        result = f_truncate(&fil_fat);
        if (result != FR_OK)
            printf("?Unable to truncate file (%d)\n", result);
    }
    if (result == FR_OK)
    {
        UINT bytes_written;
        result = f_write(&fil_fat, mbuf, mbuf_len, &bytes_written);
        if (result != FR_OK)
            printf("?Unable to write file (%d)\n", result);
    }
    if (result == FR_OK)
        fil_state = FIL_COMMAND;
    else
        fil_state = FIL_IDLE;
    fil_keep_alive();
    return true;
}

void fil_task()
{
    if (fil_state != FIL_IDLE)
        if (absolute_time_diff_us(get_absolute_time(), watchdog) < 0)
        {
            if (fil_state == FIL_COMMAND)
                puts("");
            printf("?timeout\n");
            fil_state = FIL_IDLE;
            mon_reset();
        }

    // Close file after reset or error condition
    if (fil_state == FIL_IDLE && fil_fat.obj.fs)
    {
        FRESULT result = f_close(&fil_fat);
        if (result != FR_OK)
            printf("?Unable to close file (%d)\n", result);
    }
}

void fil_keep_alive()
{
    watchdog = delayed_by_us(get_absolute_time(),
                             TIMEOUT_MS * 1000);
}

bool fil_is_prompting()
{
    return fil_state == FIL_COMMAND;
}

bool fil_is_rx_binary()
{
    return fil_state == FIL_BINARY;
}

void fil_reset()
{
    fil_state = FIL_IDLE;
}
