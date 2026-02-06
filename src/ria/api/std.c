/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/api.h"
#include "api/std.h"
#include "str/rln.h"
#include "sys/com.h"
#include "sys/pix.h"
#include "net/mdm.h"
#include "usb/cdc.h"
#include "fatfs/ff.h"
#include <stdio.h>

#if defined(DEBUG_RIA_API) || defined(DEBUG_RIA_API_STD)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

typedef enum
{
    STD_IO_ERROR,
    STD_IO_PENDING,
    STD_IO_COMPLETE,
} std_io_result_t;

typedef struct
{
    bool is_open;
    void (*close)(void);
    void (*lseek)(void);
    void (*sync)(void);
    std_io_result_t (*read)(void);
    std_io_result_t (*write)(void);
    FIL *fatfs;
} std_fd_t;

// File descriptors
#define STD_FD_MAX 16
static std_fd_t std_fd_pool[STD_FD_MAX];

// Reserved file descriptors
#define STD_FD_STDIN 0
#define STD_FD_STDOUT 1
#define STD_FD_STDERR 2
#define STD_FD_FIRST_FREE 3

// FatFs files
#define STD_FIL_MAX 8
static FIL std_fil_pool[STD_FIL_MAX];

// Active operation state
static std_fd_t *std_fd;
static char *std_buf;
static uint16_t std_len;
static uint16_t std_pos;
static int32_t std_pix;

// Readline state for stdin
static bool std_rln_active;
static const char *std_rln_buf;
static bool std_rln_needs_nl;
static size_t std_rln_pos;
static size_t std_rln_len;

static int std_find_free_fd(void)
{
    for (int fd = STD_FD_FIRST_FREE; fd < STD_FD_MAX; fd++)
        if (!std_fd_pool[fd].is_open)
            return fd;
    return -1;
}

static std_fd_t *std_validate_fd(int fd)
{
    if (fd < 0 || fd >= STD_FD_MAX || !std_fd_pool[fd].is_open)
        return NULL;
    return &std_fd_pool[fd];
}

static FIL *std_find_free_fil(void)
{
    for (int i = 0; i < STD_FIL_MAX; i++)
        if (!std_fil_pool[i].obj.fs)
            return &std_fil_pool[i];
    return NULL;
}

static void std_not_implemented(void)
{
    api_return_errno(API_ENOSYS);
}

static void std_rln_callback(bool timeout, const char *buf, size_t length)
{
    std_rln_active = false;
    if (!timeout)
    {
        std_rln_buf = buf;
        std_rln_pos = 0;
        std_rln_len = length;
        std_rln_needs_nl = true;
    }
}

static std_io_result_t std_stdin_read(void)
{
    if (!(std_rln_needs_nl || std_rln_pos < std_rln_len))
    {
        if (!std_rln_active)
        {
            std_rln_active = true;
            rln_read_line(std_rln_callback);
        }
        return STD_IO_PENDING;
    }
    size_t i;
    for (i = 0; i < std_len && std_rln_pos < std_rln_len; i++)
        std_buf[i] = std_rln_buf[std_rln_pos++];
    if (i < std_len && std_rln_needs_nl)
    {
        std_buf[i++] = '\n';
        std_rln_needs_nl = false;
    }
    std_pos = i;
    return STD_IO_COMPLETE;
}

static std_io_result_t std_stdout_write(void)
{
    while (std_pos < std_len && com_putchar_ready())
        putchar(std_buf[std_pos++]);
    return (std_pos >= std_len) ? STD_IO_COMPLETE : STD_IO_PENDING;
}

// CDC (USB serial) handlers

static int std_cdc_desc_idx;

static void std_cdc_close(void)
{
    if (cdc_close(std_cdc_desc_idx))
        api_return_ax(0);
    else
        api_return_errno(API_EIO);
}

static std_io_result_t std_cdc_read(void)
{
    int r = cdc_rx(std_cdc_desc_idx, &std_buf[std_pos], std_len - std_pos);
    if (r < 0)
    {
        api_return_errno(API_EIO);
        return STD_IO_ERROR;
    }
    std_pos += r;
    return STD_IO_COMPLETE;
}

static std_io_result_t std_cdc_write(void)
{
    int w = cdc_tx(std_cdc_desc_idx, &std_buf[std_pos], std_len - std_pos);
    if (w < 0)
    {
        api_return_errno(API_EIO);
        return STD_IO_ERROR;
    }
    std_pos += w;
    return (std_pos >= std_len) ? STD_IO_COMPLETE : STD_IO_PENDING;
}

static void std_cdc_open(const TCHAR *path, int fd)
{
    int desc_idx = cdc_open(path);
    if (desc_idx < 0)
        return;
    std_cdc_desc_idx = desc_idx;
    std_fd_pool[fd].is_open = true;
    std_fd_pool[fd].close = std_cdc_close;
    std_fd_pool[fd].read = std_cdc_read;
    std_fd_pool[fd].write = std_cdc_write;
}

static void std_mdm_close(void)
{
    if (mdm_close())
        api_return_ax(0);
    else
        api_return_errno(API_EIO);
}

static std_io_result_t std_mdm_read(void)
{
    while (std_pos < std_len)
    {
        int r = mdm_rx(&std_buf[std_pos]);
        if (r == 0)
            break;
        if (r == -1)
        {
            api_return_errno(API_EIO);
            return STD_IO_ERROR;
        }
        std_pos++;
    }
    return STD_IO_COMPLETE;
}

static std_io_result_t std_mdm_write(void)
{
    while (std_pos < std_len)
    {
        int tx = mdm_tx(std_buf[std_pos]);
        if (tx == -1)
        {
            api_return_errno(API_EIO);
            return STD_IO_ERROR;
        }
        if (tx == 0)
            break;
        std_pos++;
    }
    return (std_pos >= std_len) ? STD_IO_COMPLETE : STD_IO_PENDING;
}

static void std_mdm_open(const TCHAR *path, int fd)
{
    if (!mdm_open(path))
        return;
    std_fd_pool[fd].is_open = true;
    std_fd_pool[fd].close = std_mdm_close;
    std_fd_pool[fd].read = std_mdm_read;
    std_fd_pool[fd].write = std_mdm_write;
}

static void std_fatfs_close(void)
{
    FIL *fp = std_fd->fatfs;
    std_fd->fatfs = NULL;
    FRESULT fresult = f_close(fp);
    if (fresult != FR_OK)
        api_return_fresult(fresult);
    else
        api_return_ax(0);
}

static std_io_result_t std_fatfs_read(void)
{
    FIL *fp = std_fd->fatfs;
    UINT br;
    FRESULT fresult = f_read(fp, std_buf, std_len, &br);
    if (fresult != FR_OK)
    {
        api_return_fresult(fresult);
        return STD_IO_ERROR;
    }
    std_pos = br;
    return STD_IO_COMPLETE;
}

static std_io_result_t std_fatfs_write(void)
{
    FIL *fp = std_fd->fatfs;
    UINT bw;
    FRESULT fresult = f_write(fp, std_buf, std_len, &bw);
    if (fresult != FR_OK)
    {
        api_return_fresult(fresult);
        return STD_IO_ERROR;
    }
    std_pos = bw;
    return STD_IO_COMPLETE;
}

static void std_fatfs_lseek(void)
{
    int8_t whence;
    int32_t ofs;
    if (!api_pop_int8(&whence) || !api_pop_int32_end(&ofs))
    {
        api_return_errno(API_EINVAL);
        return;
    }
    FIL *fp = std_fd->fatfs;
    if (whence == SEEK_SET)
        ;
    else if (whence == SEEK_CUR)
        ofs += f_tell(fp);
    else if (whence == SEEK_END)
        ofs += f_size(fp);
    else
    {
        api_return_errno(API_EINVAL);
        return;
    }
    FRESULT fresult = f_lseek(fp, ofs);
    if (fresult != FR_OK)
    {
        api_return_fresult(fresult);
        return;
    }
    FSIZE_t pos = f_tell(fp);
    if (pos > 0x7FFFFFFF)
        pos = 0x7FFFFFFF;
    api_return_axsreg(pos);
}

static void std_fatfs_sync(void)
{
    FIL *fp = std_fd->fatfs;
    FRESULT fresult = f_sync(fp);
    if (fresult != FR_OK)
        api_return_fresult(fresult);
    else
        api_return_ax(0);
}

static void std_fatfs_open(const TCHAR *path, uint8_t flags, int fd)
{
    const unsigned char RDWR = 0x03;
    const unsigned char CREAT = 0x10;
    const unsigned char TRUNC = 0x20;
    const unsigned char APPEND = 0x40;
    const unsigned char EXCL = 0x80;
    uint8_t mode = flags & RDWR;
    if (flags & CREAT)
    {
        if (flags & EXCL)
            mode |= FA_CREATE_NEW;
        else if (flags & TRUNC)
            mode |= FA_CREATE_ALWAYS;
        else if (flags & APPEND)
            mode |= FA_OPEN_APPEND;
        else
            mode |= FA_OPEN_ALWAYS;
    }
    FIL *fp = std_find_free_fil();
    if (!fp)
        return;
    FRESULT fresult = f_open(fp, path, mode);
    if (fresult != FR_OK)
        return;
    std_fd_pool[fd].is_open = true;
    std_fd_pool[fd].close = std_fatfs_close;
    std_fd_pool[fd].read = std_fatfs_read;
    std_fd_pool[fd].write = std_fatfs_write;
    std_fd_pool[fd].lseek = std_fatfs_lseek;
    std_fd_pool[fd].sync = std_fatfs_sync;
    std_fd_pool[fd].fatfs = fp;
}

bool std_api_open(void)
{
    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    int fd = std_find_free_fd();
    if (fd < 0)
        return api_return_errno(API_EMFILE);

    std_fd_pool[fd].close = std_not_implemented;
    std_fd_pool[fd].read = NULL;
    std_fd_pool[fd].write = NULL;
    std_fd_pool[fd].lseek = std_not_implemented;
    std_fd_pool[fd].sync = std_not_implemented;
    std_fd_pool[fd].fatfs = NULL;

    // Check special devices first
    std_mdm_open(path, fd);
    if (std_fd_pool[fd].is_open)
        return api_return_ax(fd);

    std_cdc_open(path, fd);
    if (std_fd_pool[fd].is_open)
        return api_return_ax(fd);

    // Everything else might be a file
    uint8_t flags = API_A;
    std_fatfs_open(path, flags, fd);
    if (std_fd_pool[fd].is_open)
        return api_return_ax(fd);

    return api_return_errno(API_ENOENT);
}

bool std_api_close(void)
{
    int fd = API_A;
    if (fd < STD_FD_FIRST_FREE || fd >= STD_FD_MAX || !std_fd_pool[fd].is_open)
        return api_return_errno(API_EBADF);
    std_fd = &std_fd_pool[fd];
    std_fd->close();
    std_fd->is_open = false;
    std_fd = NULL;
    return false;
}

bool std_api_read_xstack(void)
{
    if (std_fd)
    {
        switch (std_fd->read())
        {
        case STD_IO_ERROR:
            std_fd = NULL;
            return false;
        case STD_IO_PENDING:
            return api_working();
        case STD_IO_COMPLETE:
            // relocate buffer in xstack
            uint8_t *buf = (uint8_t *)std_buf;
            uint16_t count = std_len;
            xstack_ptr = XSTACK_SIZE;
            if (std_pos == count)
                xstack_ptr -= count;
            else
                for (uint16_t i = std_pos; i;)
                    xstack[--xstack_ptr] = buf[--i];
            std_fd = NULL;
            return api_return_ax(std_pos);
        }
    }
    uint16_t count;
    int fd = API_A;
    if (!api_pop_uint16_end(&count) || count > XSTACK_SIZE)
        return api_return_errno(API_EINVAL);
    std_fd = std_validate_fd(fd);
    if (!std_fd)
        return api_return_errno(API_EBADF);
    if (!std_fd->read)
        return api_return_errno(API_ENOSYS);
    std_buf = (char *)&xstack[XSTACK_SIZE - count];
    std_len = count;
    std_pos = 0;
    return api_working();
}

bool std_api_read_xram(void)
{
    if (std_fd)
    {
        if (std_pix >= 0)
        {
            // send xram result to pix devices
            uint16_t xram_addr = std_buf - (char *)xram;
            for (; std_pix > 0 && pix_ready(); --std_pix, ++xram_addr, ++std_buf)
                pix_send(PIX_DEVICE_XRAM, 0, xram[xram_addr], xram_addr);
            if (std_pix <= 0)
            {
                std_pix = -1;
                std_fd = NULL;
                return api_return_ax(std_pos);
            }
            return api_working();
        }
        switch (std_fd->read())
        {
        case STD_IO_ERROR:
            std_fd = NULL;
            return false;
        case STD_IO_PENDING:
            return api_working();
        case STD_IO_COMPLETE:
            std_pix = std_pos;
            return api_working();
        }
    }
    uint16_t count, xram_addr;
    int fd = API_A;
    if (!api_pop_uint16(&count) || !api_pop_uint16_end(&xram_addr))
        return api_return_errno(API_EINVAL);
    std_fd = std_validate_fd(fd);
    if (!std_fd)
        return api_return_errno(API_EBADF);
    if (!std_fd->read)
        return api_return_errno(API_ENOSYS);
    if (count > 0x7FFF)
        count = 0x7FFF;
    std_buf = (char *)&xram[xram_addr];
    if (std_buf + count > (char *)xram + 0x10000)
        return api_return_errno(API_EINVAL);
    std_len = count;
    std_pos = 0;
    return api_working();
}

bool std_api_write_xstack(void)
{
    if (std_fd)
    {
        switch (std_fd->write())
        {
        case STD_IO_ERROR:
            std_fd = NULL;
            return false;
        case STD_IO_PENDING:
            return api_working();
        case STD_IO_COMPLETE:
            std_fd = NULL;
            return api_return_ax(std_pos);
        }
    }
    int fd = API_A;
    std_fd = std_validate_fd(fd);
    if (!std_fd)
        return api_return_errno(API_EBADF);
    if (!std_fd->write)
        return api_return_errno(API_ENOSYS);
    std_len = XSTACK_SIZE - xstack_ptr;
    std_buf = (char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    std_pos = 0;
    return api_working();
}

bool std_api_write_xram(void)
{
    if (std_fd)
    {
        switch (std_fd->write())
        {
        case STD_IO_ERROR:
            std_fd = NULL;
            return false;
        case STD_IO_PENDING:
            return api_working();
        case STD_IO_COMPLETE:
            std_fd = NULL;
            return api_return_ax(std_pos);
        }
    }
    uint16_t xram_addr, count;
    int fd = API_A;
    std_fd = std_validate_fd(fd);
    if (!std_fd)
        return api_return_errno(API_EBADF);
    if (!std_fd->write)
        return api_return_errno(API_ENOSYS);
    if (!api_pop_uint16(&count) || !api_pop_uint16_end(&xram_addr))
        return api_return_errno(API_EINVAL);
    if (count > 0x7FFF)
        count = 0x7FFF;
    std_buf = (char *)&xram[xram_addr];
    if (std_buf + count > (char *)xram + 0x10000)
        return api_return_errno(API_EINVAL);
    std_len = count;
    std_pos = 0;
    return api_working();
}

bool std_api_lseek_cc65(void)
{
    int8_t whence_cc65;
    int32_t ofs;
    int fd = API_A;
    std_fd = std_validate_fd(fd);
    if (!std_fd)
        return api_return_errno(API_EBADF);
    if (!api_pop_int8(&whence_cc65) || !api_pop_int32_end(&ofs))
        return api_return_errno(API_EINVAL);
    // Translate cc65 whence (2=SET, 0=CUR, 1=END)
    // to standard (0=SET, 1=CUR, 2=END)
    int8_t whence_std;
    if (whence_cc65 == 2)
        whence_std = SEEK_SET;
    else if (whence_cc65 == 0)
        whence_std = SEEK_CUR;
    else if (whence_cc65 == 1)
        whence_std = SEEK_END;
    else
        return api_return_errno(API_EINVAL);
    // Push back in standard format for the fd operation
    if (!api_push_int32(&ofs) || !api_push_int8(&whence_std))
        return api_return_errno(API_EINVAL);
    std_fd->lseek();
    std_fd = NULL;
    return false;
}

bool std_api_lseek_llvm(void)
{
    int fd = API_A;
    std_fd = std_validate_fd(fd);
    if (!std_fd)
        return api_return_errno(API_EBADF);
    std_fd->lseek();
    std_fd = NULL;
    return false;
}

bool std_api_syncfs(void)
{
    int fd = API_A;
    std_fd = std_validate_fd(fd);
    if (!std_fd)
        return api_return_errno(API_EBADF);
    std_fd->sync();
    std_fd = NULL;
    return false;
}

void std_run(void)
{
    std_fd = NULL;
    std_pix = -1;
    std_rln_active = false;
    std_rln_needs_nl = false;
    std_rln_pos = 0;
    std_rln_len = 0;

    for (int i = 0; i < STD_FD_MAX; i++)
    {
        std_fd_pool[i].is_open = false;
        std_fd_pool[i].close = std_not_implemented;
        std_fd_pool[i].lseek = std_not_implemented;
        std_fd_pool[i].sync = std_not_implemented;
        std_fd_pool[i].read = NULL;
        std_fd_pool[i].write = NULL;
        std_fd_pool[i].fatfs = NULL;
    }

    std_fd_pool[STD_FD_STDIN].is_open = true;
    std_fd_pool[STD_FD_STDIN].read = std_stdin_read;

    std_fd_pool[STD_FD_STDOUT].is_open = true;
    std_fd_pool[STD_FD_STDOUT].write = std_stdout_write;

    std_fd_pool[STD_FD_STDERR].is_open = true;
    std_fd_pool[STD_FD_STDERR].write = std_stdout_write;
}

void std_stop(void)
{
    for (int i = STD_FD_FIRST_FREE; i < STD_FD_MAX; i++)
    {
        if (!std_fd_pool[i].is_open)
            continue;
        std_fd = &std_fd_pool[i];
        std_fd_pool[i].close();
    }
}
