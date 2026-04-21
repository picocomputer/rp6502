/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mon/fil.h"
#include "mon/mon.h"
#include "str/rln.h"
#include "str/str.h"
#include "sys/mem.h"
#include "sys/ria.h"
#include <fatfs/ff.h>
#include <stdio.h>
#include <string.h>

#if defined(DEBUG_RIA_MON) || defined(DEBUG_RIA_MON_FIL)
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define FIL_TIMEOUT_MS 250

static enum {
    FIL_IDLE,
    FIL_COMMAND,
} fil_state;

static uint32_t fil_rx_size;
static uint32_t fil_rx_crc;
static DIR fil_fatfs_dir;
static FIL fil_fatfs_fil;

bool fil_drive_exists(const char *args)
{
    const char *tok = str_parse_string(&args);
    if (!tok)
        return false;
    size_t len = strlen(tok);
    if (len < 2 || tok[len - 1] != ':')
        return false;
    DIR dir;
    FRESULT result = f_opendir(&dir, tok);
    if (result == FR_OK)
        f_closedir(&dir);
    return result != FR_INVALID_DRIVE;
}

static int fil_cwd_response(char *buf, size_t buf_size, int state)
{
    if (state < 0)
        return state;
    FRESULT result;
    char *cwd = (char *)mbuf;
    result = f_getcwd(cwd, MBUF_SIZE);
    mon_add_response_fatfs(result);
    if (result != FR_OK)
        return -1;
    size_t width = buf_size - 2;
    size_t total = strlen(cwd);
    if (total < (size_t)state * width)
        return -1;
    cwd += (size_t)state * width;
    snprintf(buf, width + 1, "%s", cwd);
    size_t written = strlen(buf);
    if (written)
    {
        buf[written] = '\n';
        buf[written + 1] = '\0';
    }
    return state + 1;
}

void fil_mon_chdir(const char *args)
{
    FRESULT result;
    DIR dir;
    if (!*args)
    {
        mon_add_response_fn(fil_cwd_response);
        return;
    }
    const char *path = str_parse_string(&args);
    if (!path || !str_parse_end(args))
    {
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
        return;
    }
    result = f_opendir(&dir, path);
    mon_add_response_fatfs(result);
    if (result == FR_OK)
    {
        result = f_closedir(&dir);
        mon_add_response_fatfs(result);
    }
    if (result == FR_OK)
    {
        result = f_chdir(path);
        mon_add_response_fatfs(result);
    }
}

void fil_mon_mkdir(const char *args)
{
    const char *path = str_parse_string(&args);
    if (!path || !str_parse_end(args))
    {
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
        return;
    }
    FRESULT result = f_mkdir(path);
    mon_add_response_fatfs(result);
}

void fil_mon_chdrive(const char *args)
{
    const char *tok = str_parse_string(&args);
    if (!tok || !str_parse_end(args))
    {
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
        return;
    }
    DIR dir;
    FRESULT result = f_opendir(&dir, tok);
    mon_add_response_fatfs(result);
    if (result == FR_OK)
    {
        result = f_closedir(&dir);
        mon_add_response_fatfs(result);
    }
    if (result == FR_OK)
    {
        result = f_chdrive(tok);
        mon_add_response_fatfs(result);
    }
}

static int fil_dir_entry_response(char *buf, size_t buf_size, int state)
{
    if (state < 0)
    {
        f_closedir(&fil_fatfs_dir);
        fil_fatfs_dir.obj.fs = NULL;
        return state;
    }
    FILINFO fno;
    FRESULT fresult = f_readdir(&fil_fatfs_dir, &fno);
    mon_add_response_fatfs(fresult);
    if (fresult != FR_OK || fno.fname[0] == 0)
    {
        f_closedir(&fil_fatfs_dir);
        fil_fatfs_dir.obj.fs = NULL;
        return -1;
    }
    if (fno.fattrib & (AM_HID | AM_SYS))
        return 0;
    if (fno.fattrib & AM_DIR)
        snprintf(buf, buf_size, " <DIR> %.72s\n", fno.fname);
    else
    {
        double size = fno.fsize;
        if (size <= 999999)
            snprintf(buf, buf_size, "%6.0f %.72s\n", size, fno.fname);
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
            snprintf(buf, buf_size, "%5.1f%c %.72s\n", size, c, fno.fname);
        }
    }
    if (strlen(fno.fname) > 72)
    {
        buf[76] = '.';
        buf[77] = '.';
        buf[78] = '.';
    }
    return 0;
}

void fil_mon_ls(const char *args)
{
    const char *path = str_parse_string(&args);
    if (!str_parse_end(args))
    {
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
        return;
    }
    FRESULT fresult = f_opendir(&fil_fatfs_dir, path ? path : ".");
    mon_add_response_fatfs(fresult);
    if (FR_OK != fresult)
        return;
    mon_add_response_fn(fil_cwd_response);
    mon_add_response_fn(fil_dir_entry_response);
}

static void fil_upload_dispatch(bool timeout, const char *buf);

static void fil_upload_rx_mbuf(bool timeout)
{
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
        rln_read_line_timeout(fil_upload_dispatch, FIL_TIMEOUT_MS);
    }
    else
        fil_state = FIL_IDLE;
}

static void fil_upload_dispatch(bool timeout, const char *buf)
{
    if (timeout)
    {
        puts("");
        mon_add_response_str(STR_ERR_RX_TIMEOUT);
        fil_state = FIL_IDLE;
        return;
    }
    const char *scan = buf;
    const char *tok = str_parse_string(&scan);
    if (!tok)
    {
        if (!str_parse_end(scan))
            mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
        fil_state = FIL_IDLE;
        return;
    }
    if (!strcasecmp(tok, STR_END) && str_parse_end(scan))
    {
        fil_state = FIL_IDLE;
        return;
    }
    const char *args = buf;
    if (str_parse_uint32(&args, &fil_rx_size) &&
        str_parse_uint32(&args, &fil_rx_crc) &&
        str_parse_end(args))
    {
        if (!fil_rx_size || fil_rx_size > MBUF_SIZE)
        {
            fil_state = FIL_IDLE;
            mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
            return;
        }
        mem_read_mbuf(FIL_TIMEOUT_MS, fil_upload_rx_mbuf, fil_rx_size);
        return;
    }
    mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
    fil_state = FIL_IDLE;
    return;
}

void fil_mon_upload(const char *args)
{
    const char *path = str_parse_string(&args);
    if (!path || !str_parse_end(args))
    {
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
        return;
    }
    FRESULT result = f_open(&fil_fatfs_fil, path, FA_READ | FA_WRITE);
    if (result == FR_NO_FILE)
        result = f_open(&fil_fatfs_fil, path, FA_CREATE_NEW | FA_WRITE);
    if (result != FR_OK)
    {
        mon_add_response_fatfs(result);
        return;
    }
    fil_state = FIL_COMMAND;
    putchar('}');
    rln_read_line_timeout(fil_upload_dispatch, FIL_TIMEOUT_MS);
}

void fil_mon_unlink(const char *args)
{
    const char *path = str_parse_string(&args);
    if (!path || !str_parse_end(args))
    {
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
        return;
    }
    FRESULT result = f_unlink(path);
    mon_add_response_fatfs(result);
}

void fil_task(void)
{
    // Close file after reset or error condition
    if (fil_state == FIL_IDLE && fil_fatfs_fil.obj.fs)
    {
        FRESULT result = f_close(&fil_fatfs_fil);
        fil_fatfs_fil.obj.fs = NULL;
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
