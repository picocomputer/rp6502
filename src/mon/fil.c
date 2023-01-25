/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "fil.h"
#include "str.h"
#include "ria/ria.h"
#include "mem/mbuf.h"
#include "fatfs/ff.h"
#include <stdio.h>
#include "pico/stdlib.h"
// #include "hardware/clocks.h"

#define MON_BINARY_TIMEOUT_MS 200

static enum {
    FIL_IDLE,
    FIL_COMMAND,
    FIL_BINARY,
} fil_state;

// static uint32_t rw_addr;
static uint32_t rw_len;
static uint32_t rw_crc;
static absolute_time_t binary_timer;
static FIL fat_fil;

void fil_ls(const char *args, size_t len)
{
    (void)(len);

    const char *dpath = ".";
    if (args[0])
        dpath = args;

    DIR dir;
    if (FR_OK != f_opendir(&dir, dpath))
    {
        printf("?cannot access '%s': No such file or directory\n", dpath);
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

void fil_cd(const char *args, size_t len)
{
    (void)(len);

    if (!args[0])
    {
        // TODO print current directory
        printf("?invalid arguments\n");
        return;
    }
    if ((FR_OK != f_chdir(args)) || (FR_OK != f_chdrive(args)))
    {
        printf("?No such file or directory\n");
        return;
    }
}

static void fil_write_block()
{
    FRESULT result = FR_OK;

    if (mbuf_crc32() != rw_crc)
    {
        result = FR_INT_ERR; // any error to abort
        puts("?CRC does not match");
    }

    // This will let us leave the file unchanged until
    // the first chunk is received successfully.
    if (result == FR_OK && f_tell(&fat_fil) == 0)
    {
        result = f_truncate(&fat_fil);
        if (result != FR_OK)
            printf("?Unable to truncate file (%d)\n", result);
    }

    if (result == FR_OK)
    {
        UINT bytes_written;
        result = f_write(&fat_fil, mbuf, mbuf_len, &bytes_written);
        if (result != FR_OK)
            printf("?Unable to write file (%d)\n", result);
    }

    if (result == FR_OK)
        fil_state = FIL_COMMAND;
    else
        fil_state = FIL_IDLE;
}

void fil_dispatch(const char *args, size_t len)
{
    if (len == 0 || (len == 3 && !strnicmp("END", args, 3)))
    {
        fil_state = FIL_IDLE;
        FRESULT result = f_close(&fat_fil);
        if (result != FR_OK)
            printf("?Unable to close file (%d)\n", result);
        return;
    }

    if (parse_uint32(&args, &len, &rw_len) &&
        parse_uint32(&args, &len, &rw_crc) &&
        parse_end(args, len))
    {
        if (!rw_len || rw_len > MBUF_SIZE)
        {
            printf("?invalid length\n");
            return;
        }
        mbuf_len = 0;
        fil_state = FIL_BINARY;
        binary_timer = delayed_by_us(get_absolute_time(),
                                     MON_BINARY_TIMEOUT_MS * 1000);

        return;
    }
    printf("?invalid argument\n");
    return;
}

void fil_upload(const char *args, size_t len)
{
    if (len == 0)
    {
        printf("?missing filename\n");
        return;
    }

    FRESULT result = f_open(&fat_fil, args, FA_READ | FA_WRITE);
    if (result == FR_NO_FILE)
        result = f_open(&fat_fil, args, FA_CREATE_NEW | FA_WRITE);
    if (result != FR_OK)
    {
        printf("?Unable to open file (%d)\n", result);
        return;
    }
    fil_state = FIL_COMMAND;
    binary_timer = delayed_by_us(get_absolute_time(),
                                 MON_BINARY_TIMEOUT_MS * 1000);
}

void fil_binary_handler()
{
    int ch = getchar_timeout_us(0);
    if (ch != PICO_ERROR_TIMEOUT)
    {
        while (ch != PICO_ERROR_TIMEOUT)
        {
            mbuf[mbuf_len++] = ch;
            if (mbuf_len >= rw_len)
            {
                fil_write_block();
                return;
            }
            ch = getchar_timeout_us(0);
        }
        binary_timer = delayed_by_us(get_absolute_time(),
                                     MON_BINARY_TIMEOUT_MS * 1000);
    }
    if (absolute_time_diff_us(get_absolute_time(), binary_timer) < 0)
    {
        fil_state = FIL_IDLE;
        printf("?timeout\n");
    }
}

void fil_task()
{
    // Close file after reset or error condition
    if (fil_state == FIL_IDLE && fat_fil.obj.fs)
    {
        FRESULT result = f_close(&fat_fil);
        if (result != FR_OK)
            printf("?Unable to close file (%d)\n", result);
    }
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
