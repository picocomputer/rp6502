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
#include <assert.h>
#include <fatfs/ff.h>
#include <stdio.h>
#include <string.h>

#if defined(DEBUG_RIA_MON) || defined(DEBUG_RIA_MON_FIL)
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define FIL_TIMEOUT_MS 500

static enum {
    FIL_IDLE,
    FIL_COMMAND,
} fil_state;

static uint32_t fil_rx_size;
static uint32_t fil_rx_crc;
static DIR fil_fatfs_dir;
static FIL fil_fatfs_fil;

// COPY and MOVE put two paths in xstack.
static_assert(2 * (FF_LFN_BUF + 1) <= XSTACK_SIZE);

static FRESULT fil_resolve_dst(const char *src, const char *dst,
                               char *out, size_t out_sz)
{
    // Use f_opendir rather than f_stat to detect the directory case;
    // f_stat returns FR_INVALID_NAME for "." and "..".
    DIR dir;
    if (f_opendir(&dir, dst) == FR_OK)
    {
        f_closedir(&dir);
        // Use src's on-disk basename so case is preserved.
        char fname[FF_LFN_BUF + 1];
        if (!str_lookup_basename(src, fname, sizeof fname))
            return FR_NO_FILE;
        // Skip the joiner if dst already ends in a separator.
        size_t dst_len = strlen(dst);
        const char *sep = (dst_len > 0 && str_is_sep(dst[dst_len - 1])) ? "" : "/";
        int n = snprintf(out, out_sz, "%s%s%s", dst, sep, fname);
        if (n < 0 || (size_t)n >= out_sz)
            return FR_INVALID_NAME;
        return FR_OK;
    }
    // dst doesn't resolve to a directory; pass it through. The caller's
    // f_open / f_rename will report any real error.
    size_t len = strlen(dst);
    if (len + 1 > out_sz)
        return FR_INVALID_NAME;
    memcpy(out, dst, len + 1);
    return FR_OK;
}

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
    // Split the path at the visible terminal width so long cwds wrap
    // cleanly on a 40-col display instead of overflowing past the edge.
    // The +2 budget covers the trailing newline and null terminator.
    size_t width = rln_get_term_width();
    if (width > buf_size - 2)
        width = buf_size - 2;
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
    // 7-char fixed prefix (" <DIR> ", "%6.0f ", or "%5.1f%c ") before the name.
    int name_max = (int)rln_get_term_width() - 7;
    if (name_max > (int)buf_size - 9)
        name_max = (int)buf_size - 9;
    if (fno.fattrib & AM_DIR)
        snprintf(buf, buf_size, " <DIR> %.*s\n", name_max, fno.fname);
    else
    {
        double size = fno.fsize;
        if (size <= 999999)
            snprintf(buf, buf_size, "%6.0f %.*s\n", size, name_max, fno.fname);
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
            snprintf(buf, buf_size, "%5.1f%c %.*s\n", size, c, name_max, fno.fname);
        }
    }
    if (strlen(fno.fname) > (size_t)name_max)
    {
        buf[7 + name_max - 3] = '.';
        buf[7 + name_max - 2] = '.';
        buf[7 + name_max - 1] = '.';
    }
    return 0;
}

void fil_mon_dir(const char *args)
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

void fil_mon_copy(const char *args)
{
    char *src_path = (char *)&xstack[0];
    char *dst_path = (char *)&xstack[FF_LFN_BUF + 1];
    const char *src_raw = str_parse_string(&args);
    if (src_raw)
    {
        strncpy(src_path, src_raw, FF_LFN_BUF + 1);
        src_path[FF_LFN_BUF] = '\0';
    }
    const char *dst = str_parse_string(&args);
    if (!src_raw || !dst || !str_parse_end(args))
    {
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
        return;
    }
    FRESULT fr = fil_resolve_dst(src_path, dst, dst_path, FF_LFN_BUF + 1);
    if (fr != FR_OK)
    {
        mon_add_response_fatfs(fr);
        return;
    }
    fr = f_open(&fil_fatfs_fil, src_path, FA_READ);
    if (fr != FR_OK)
    {
        mon_add_response_fatfs(fr);
        return;
    }
    FIL dst_fil;
    fr = f_open(&dst_fil, dst_path, FA_CREATE_NEW | FA_WRITE);
    if (fr != FR_OK)
    {
        f_close(&fil_fatfs_fil);
        mon_add_response_fatfs(fr);
        return;
    }
    UINT br, bw;
    for (;;)
    {
        fr = f_read(&fil_fatfs_fil, mbuf, MBUF_SIZE, &br);
        if (fr != FR_OK || br == 0)
            break;
        fr = f_write(&dst_fil, mbuf, br, &bw);
        if (fr != FR_OK)
            break;
        if (bw < br)
        {
            fr = FR_DISK_ERR;
            break;
        }
    }
    f_close(&fil_fatfs_fil);
    f_close(&dst_fil);
    mon_add_response_fatfs(fr);
}

void fil_mon_move(const char *args)
{
    char *src_path = (char *)&xstack[0];
    char *dst_path = (char *)&xstack[FF_LFN_BUF + 1];
    const char *src_raw = str_parse_string(&args);
    if (src_raw)
    {
        strncpy(src_path, src_raw, FF_LFN_BUF + 1);
        src_path[FF_LFN_BUF] = '\0';
    }
    const char *dst = str_parse_string(&args);
    if (!src_raw || !dst || !str_parse_end(args))
    {
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
        return;
    }
    FRESULT fr = fil_resolve_dst(src_path, dst, dst_path, FF_LFN_BUF + 1);
    if (fr != FR_OK)
    {
        mon_add_response_fatfs(fr);
        return;
    }
    mon_add_response_fatfs(f_rename(src_path, dst_path));
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
