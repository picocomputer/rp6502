/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mon/fil.h"
#include "mon/mon.h"
#include "str/str.h"
#include "sys/mem.h"
#include "sys/ria.h"
#include "sys/rln.h"
#include <fatfs/ff.h>
#include <stdio.h>
#include <string.h>

#if defined(DEBUG_RIA_MON) || defined(DEBUG_RIA_MON_FIL)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define FIL_TIMEOUT_MS 200

static enum {
    FIL_IDLE,
    FIL_COMMAND,
} fil_state;

static uint32_t fil_rx_len;
static uint32_t fil_rx_crc;
static DIR fil_fatfs_dir;
static FIL fil_fatfs_fil;

static int fil_chdir_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    char s[buf_size - 1];
    FRESULT result;
    result = f_getcwd(s, sizeof(s));
    mon_add_response_fatfs(result);
    if (result == FR_OK)
        snprintf(buf, buf_size, "%s\n", s);
    return -1;
}

void fil_mon_chdir(const char *args, size_t len)
{
    FRESULT result;
    DIR dir;
    if (!len)
    {
        mon_add_response_fn(fil_chdir_response);
        return;
    }
    result = f_opendir(&dir, args);
    mon_add_response_fatfs(result);
    if (result == FR_OK)
    {
        result = f_closedir(&dir);
        mon_add_response_fatfs(result);
    }
    if (result == FR_OK)
    {
        result = f_chdir(args);
        mon_add_response_fatfs(result);
    }
}

void fil_mon_mkdir(const char *args, size_t len)
{
    FRESULT result;
    if (!len)
    {
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
        return;
    }
    result = f_mkdir(args);
    mon_add_response_fatfs(result);
}

void fil_mon_chdrive(const char *args, size_t len)
{
    (void)len;
    FRESULT result = FR_INVALID_DRIVE;
    DIR dir;
    char s[7]; // up to "USB99:\0"
    if (len &&
        str_parse_string(&args, &len, s, sizeof(s)) &&
        str_parse_end(args, len))
    {
        result = f_opendir(&dir, s);
    }
    mon_add_response_fatfs(result);
    if (result == FR_OK)
    {
        result = f_closedir(&dir);
        mon_add_response_fatfs(result);
    }
    if (result == FR_OK)
    {
        result = f_chdrive(s);
        mon_add_response_fatfs(result);
    }
}

static int fil_dir_entry_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    if (state < 0)
    {
        f_closedir(&fil_fatfs_dir);
        return state;
    }
    FILINFO fno;
    FRESULT fresult = f_readdir(&fil_fatfs_dir, &fno);
    mon_add_response_fatfs(fresult);
    if (fresult != FR_OK || fno.fname[0] == 0)
    {
        f_closedir(&fil_fatfs_dir);
        return -1;
    }
    if (fno.fattrib & (AM_HID | AM_SYS))
        /* nop */;
    else if (fno.fattrib & AM_DIR)
        snprintf(buf, buf_size, " <DIR> %s\n", fno.fname);
    else
    {
        double size = fno.fsize;
        if (size <= 999999)
            snprintf(buf, buf_size, "%6.0f %s\n", size, fno.fname);
        else
        {
            size /= 1024;
            char c = 'K';
            if (size >= 1000)
                size /= 1024, c = 'M';
            if (size >= 1000)
                size /= 1024, c = 'G';
            if (size >= 1000)
                size /= 1024, c = 'T';
            snprintf(buf, buf_size, "%5.1f%c %s\n", size, c, fno.fname);
        }
    }
    return 0;
}

void fil_mon_ls(const char *args, size_t len)
{
    const char *dpath = ".";
    if (len)
        dpath = args;
    FRESULT fresult = f_opendir(&fil_fatfs_dir, dpath);
    mon_add_response_fatfs(fresult);
    if (FR_OK != fresult)
        return;
    mon_add_response_fn(fil_dir_entry_response);
}

static void fil_command_dispatch(bool timeout, const char *buf, size_t len);

static void fil_com_rx_mbuf(bool timeout, const char *buf, size_t length)
{
    (void)buf;
    mbuf_len = length;
    FRESULT result = FR_OK;
    if (timeout)
    {
        result = FR_INT_ERR;
        mon_add_response_str(STR_ERR_RX_TIMEOUT);
    }
    else if (ria_buf_crc32() != fil_rx_crc)
    {
        result = FR_INT_ERR;
        mon_add_response_str(STR_ERR_CRC);
    }
    // This will leave the file unchanged until
    // the first chunk is received successfully.
    if (result == FR_OK && f_tell(&fil_fatfs_fil) == 0)
    {
        result = f_truncate(&fil_fatfs_fil);
        mon_add_response_fatfs(result);
    }
    if (result == FR_OK)
    {
        UINT bytes_written;
        result = f_write(&fil_fatfs_fil, mbuf, mbuf_len, &bytes_written);
        mon_add_response_fatfs(result);
    }
    if (result == FR_OK)
    {
        fil_state = FIL_COMMAND;
        putchar('}');
        rln_read_line(FIL_TIMEOUT_MS, fil_command_dispatch, 79, 0);
    }
    else
        fil_state = FIL_IDLE;
}

static void fil_command_dispatch(bool timeout, const char *buf, size_t len)
{
    if (timeout)
    {
        puts("");
        mon_add_response_str(STR_ERR_RX_TIMEOUT);
        fil_state = FIL_IDLE;
        return;
    }
    const char *args = buf;
    if (len == 0 || (len == 3 && !strncasecmp(STR_END, args, 3)))
    {
        fil_state = FIL_IDLE;
        FRESULT result = f_close(&fil_fatfs_fil);
        mon_add_response_fatfs(result);
        return;
    }
    if (str_parse_uint32(&args, &len, &fil_rx_len) &&
        str_parse_uint32(&args, &len, &fil_rx_crc) &&
        str_parse_end(args, len))
    {
        if (!fil_rx_len || fil_rx_len > MBUF_SIZE)
        {
            fil_state = FIL_IDLE;
            mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
            return;
        }
        rln_read_binary(FIL_TIMEOUT_MS, fil_com_rx_mbuf, mbuf, fil_rx_len);
        return;
    }
    mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
    fil_state = FIL_IDLE;
    return;
}

void fil_mon_upload(const char *args, size_t len)
{
    if (!len)
    {
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
        return;
    }
    FRESULT result = f_open(&fil_fatfs_fil, args, FA_READ | FA_WRITE);
    if (result == FR_NO_FILE)
        result = f_open(&fil_fatfs_fil, args, FA_CREATE_NEW | FA_WRITE);
    if (result != FR_OK)
    {
        mon_add_response_fatfs(result);
        return;
    }
    fil_state = FIL_COMMAND;
    putchar('}');
    rln_read_line(FIL_TIMEOUT_MS, fil_command_dispatch, 79, 0);
}

void fil_mon_unlink(const char *args, size_t len)
{
    (void)(len);
    FRESULT result = f_unlink(args);
    mon_add_response_fatfs(result);
}

void fil_task(void)
{
    // Close file after reset or error condition
    if (fil_state == FIL_IDLE && fil_fatfs_fil.obj.fs)
    {
        FRESULT result = f_close(&fil_fatfs_fil);
        mon_add_response_fatfs(result);
    }
}

bool fil_active(void)
{
    return fil_state == FIL_COMMAND;
}

void fil_break(void)
{
    fil_state = FIL_IDLE;
}
