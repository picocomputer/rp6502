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
#include "usb/msc.h"
#include <stdio.h>

#if defined(DEBUG_RIA_API) || defined(DEBUG_RIA_API_STD)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// Driver table, msc must be last
typedef struct
{
    bool (*handles)(const char *);
    int (*open)(const char *, uint8_t);
    bool (*close)(int);
    int (*read)(int, char *, int);
    int (*write)(int, const char *, int);
    int32_t (*lseek)(int, int8_t, int32_t);
    bool (*sync)(int);
} std_driver_t;
static const std_driver_t drivers[] = {
    {mdm_std_handles, mdm_std_open, mdm_std_close, mdm_std_read, mdm_std_write, NULL, NULL},
    {cdc_std_handles, cdc_std_open, cdc_std_close, cdc_std_read, cdc_std_write, NULL, NULL},
    {msc_std_handles, msc_std_open, msc_std_close, msc_std_read, msc_std_write, msc_std_lseek, msc_std_sync},
};
#define STD_DRIVER_COUNT (sizeof(drivers) / sizeof(drivers[0]))

// File descriptors
#define STD_FD_MAX 16
typedef struct
{
    bool is_open;
    bool (*close)(int);
    int (*read)(int, char *, int);
    int (*write)(int, const char *, int);
    int32_t (*lseek)(int, int8_t, int32_t);
    bool (*sync)(int);
    int idx;
} std_fd_t;
static std_fd_t std_fd_pool[STD_FD_MAX];

// Reserved file descriptors
#define STD_FD_STDIN 0
#define STD_FD_STDOUT 1
#define STD_FD_STDERR 2
#define STD_FD_FIRST_FREE 3

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

static int std_stdin_read(int idx, char *buf, int count)
{
    (void)idx;
    if (!(std_rln_needs_nl || std_rln_pos < std_rln_len))
    {
        if (!std_rln_active)
        {
            std_rln_active = true;
            rln_read_line(std_rln_callback);
        }
        return -2; // pending
    }
    int i;
    for (i = 0; i < count && std_rln_pos < std_rln_len; i++)
        buf[i] = std_rln_buf[std_rln_pos++];
    if (i < count && std_rln_needs_nl)
    {
        buf[i++] = '\n';
        std_rln_needs_nl = false;
    }
    return i;
}

static int std_stdout_write(int idx, const char *buf, int count)
{
    (void)idx;
    int i;
    for (i = 0; i < count && com_putchar_ready(); i++)
        putchar(buf[i]);
    return i;
}

bool std_api_open(void)
{
    char *path = (char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    int fd = std_find_free_fd();
    if (fd < 0)
        return api_return_errno(API_EMFILE);

    std_fd_pool[fd].idx = -1;

    uint8_t flags = API_A;
    for (size_t i = 0; i < STD_DRIVER_COUNT; i++)
    {
        if (drivers[i].handles(path))
        {
            int idx = drivers[i].open(path, flags);
            if (idx >= 0)
            {
                std_fd_pool[fd].is_open = true;
                std_fd_pool[fd].close = drivers[i].close;
                std_fd_pool[fd].read = drivers[i].read;
                std_fd_pool[fd].write = drivers[i].write;
                std_fd_pool[fd].lseek = drivers[i].lseek;
                std_fd_pool[fd].sync = drivers[i].sync;
                std_fd_pool[fd].idx = idx;
                return api_return_ax(fd);
            }
            else
                return api_return_errno(API_EIO);
        }
    }
    return api_return_errno(API_ENOENT);
}

bool std_api_close(void)
{
    int fd = API_A;
    if (fd < STD_FD_FIRST_FREE || fd >= STD_FD_MAX || !std_fd_pool[fd].is_open)
        return api_return_errno(API_EBADF);
    std_fd_t *f = &std_fd_pool[fd];
    f->is_open = false;
    if (f->close && !f->close(f->idx))
        return api_return_errno(API_EIO);
    return api_return_ax(0);
}

bool std_api_read_xstack(void)
{
    if (std_fd)
    {
        int r = std_fd->read(std_fd->idx, &std_buf[std_pos], std_len - std_pos);
        if (r < -1)
            return api_working();
        if (r < 0)
        {
            std_fd = NULL;
            return api_return_errno(API_EIO);
        }
        std_pos += r;
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
        int r = std_fd->read(std_fd->idx, &std_buf[std_pos], std_len - std_pos);
        if (r < -1)
            return api_working();
        if (r < 0)
        {
            std_fd = NULL;
            return api_return_errno(API_EIO);
        }
        std_pos += r;
        std_pix = std_pos;
        return api_working();
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
        int w = std_fd->write(std_fd->idx, &std_buf[std_pos], std_len - std_pos);
        if (w < 0)
        {
            std_fd = NULL;
            return api_return_errno(API_EIO);
        }
        std_pos += w;
        if (std_pos < std_len)
            return api_working();
        std_fd = NULL;
        return api_return_ax(std_pos);
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
        int w = std_fd->write(std_fd->idx, &std_buf[std_pos], std_len - std_pos);
        if (w < 0)
        {
            std_fd = NULL;
            return api_return_errno(API_EIO);
        }
        std_pos += w;
        if (std_pos < std_len)
            return api_working();
        std_fd = NULL;
        return api_return_ax(std_pos);
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
    std_fd_t *f = std_validate_fd(fd);
    if (!f)
        return api_return_errno(API_EBADF);
    if (!api_pop_int8(&whence_cc65) || !api_pop_int32_end(&ofs))
        return api_return_errno(API_EINVAL);
    if (!f->lseek)
        return api_return_errno(API_ENOSYS);
    // Translate cc65 whence (2=SET, 0=CUR, 1=END)
    // to standard (0=SET, 1=CUR, 2=END)
    int8_t whence;
    if (whence_cc65 == 2)
        whence = SEEK_SET;
    else if (whence_cc65 == 0)
        whence = SEEK_CUR;
    else if (whence_cc65 == 1)
        whence = SEEK_END;
    else
        return api_return_errno(API_EINVAL);
    int32_t pos = f->lseek(f->idx, whence, ofs);
    if (pos < 0)
        return api_return_errno(API_EIO);
    return api_return_axsreg(pos);
}

bool std_api_lseek_llvm(void)
{
    int8_t whence;
    int32_t ofs;
    int fd = API_A;
    std_fd_t *f = std_validate_fd(fd);
    if (!f)
        return api_return_errno(API_EBADF);
    if (!api_pop_int8(&whence) || !api_pop_int32_end(&ofs))
        return api_return_errno(API_EINVAL);
    if (!f->lseek)
        return api_return_errno(API_ENOSYS);
    int32_t pos = f->lseek(f->idx, whence, ofs);
    if (pos < 0)
        return api_return_errno(API_EIO);
    return api_return_axsreg(pos);
}

bool std_api_syncfs(void)
{
    int fd = API_A;
    std_fd_t *f = std_validate_fd(fd);
    if (!f)
        return api_return_errno(API_EBADF);
    if (!f->sync)
        return api_return_errno(API_ENOSYS);
    if (!f->sync(f->idx))
        return api_return_errno(API_EIO);
    return api_return_ax(0);
}

void std_run(void)
{
    std_fd = NULL;
    std_pix = -1;
    std_rln_active = false;
    std_rln_needs_nl = false;
    std_rln_pos = 0;
    std_rln_len = 0;

    // for (int i = 0; i < STD_FD_MAX; i++)
    // {
    //     std_fd_pool[i] = (std_fd_t){0};
    // }

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
        std_fd_pool[i].close(std_fd_pool[i].idx);
        std_fd_pool[i].is_open = false;
    }
}
