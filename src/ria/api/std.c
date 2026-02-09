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
#include "usb/vcp.h"
#include "usb/msc.h"
#include <stdio.h>

#if defined(DEBUG_RIA_API) || defined(DEBUG_RIA_API_STD)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// Driver table, msc is catch-all and must be last.
typedef struct
{
    // handles, open, and close are required
    bool (*handles)(const char *);
    int (*open)(const char *, uint8_t, api_errno *);
    int (*close)(int desc, api_errno *);
    // everything else is optional
    std_rw_result (*read)(int desc, char *, uint32_t, uint32_t *, api_errno *);
    std_rw_result (*write)(int desc, const char *, uint32_t, uint32_t *, api_errno *);
    int (*sync)(int desc, api_errno *);
    int (*lseek)(int desc, int8_t, int32_t, int32_t *, api_errno *);
} std_driver_t;
__in_flash("std_drivers") static const std_driver_t std_drivers[] = {
    {mdm_std_handles, mdm_std_open, mdm_std_close, mdm_std_read, mdm_std_write, NULL, NULL},
    {vcp_std_handles, vcp_std_open, vcp_std_close, vcp_std_read, vcp_std_write, NULL, NULL},
    {msc_std_handles, msc_std_open, msc_std_close, msc_std_read, msc_std_write, msc_std_sync, msc_std_lseek},
};
#define STD_DRIVER_COUNT (sizeof(std_drivers) / sizeof(std_drivers[0]))

// The stdio file descriptor pool.
#define STD_FD_MAX 16
#define STD_FD_STDIN 0
#define STD_FD_STDOUT 1
#define STD_FD_STDERR 2
#define STD_FD_FIRST_FREE 3
typedef struct
{
    bool is_open;
    int (*close)(int, api_errno *);
    std_rw_result (*read)(int, char *, uint32_t, uint32_t *, api_errno *);
    std_rw_result (*write)(int, const char *, uint32_t, uint32_t *, api_errno *);
    int (*sync)(int, api_errno *);
    int (*lseek)(int, int8_t, int32_t, int32_t *, api_errno *);
    int desc;
} std_fd_t;
static std_fd_t std_fd_pool[STD_FD_MAX];

// Active operation state.
static std_fd_t *std_fd;
static char *std_buf;
static uint16_t std_size;
static uint16_t std_pos;
static int32_t std_pix;

// Readline state for stdin.
static bool std_rln_active;
static const char *std_rln_buf;
static bool std_rln_needs_nl;
static size_t std_rln_pos;
static size_t std_rln_len;

static std_fd_t *std_validate_fd(int fd)
{
    if (fd < 0 || fd >= STD_FD_MAX || !std_fd_pool[fd].is_open)
        return NULL;
    return &std_fd_pool[fd];
}

static void std_rln_callback(bool timeout, const char *buf, size_t length)
{
    (void)timeout;
    std_rln_active = false;
    std_rln_buf = buf;
    std_rln_pos = 0;
    std_rln_len = length;
    std_rln_needs_nl = true;
}

static std_rw_result std_stdin_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err)
{
    (void)desc;
    (void)err;
    if (!(std_rln_needs_nl || std_rln_pos < std_rln_len))
    {
        if (!std_rln_active)
        {
            std_rln_active = true;
            rln_read_line(std_rln_callback);
        }
        *bytes_read = 0;
        return STD_PENDING;
    }
    uint32_t i = 0;
    for (; i < count && std_rln_pos < std_rln_len; i++)
        buf[i] = std_rln_buf[std_rln_pos++];
    if (i < count && std_rln_needs_nl)
    {
        buf[i++] = '\n';
        std_rln_needs_nl = false;
    }
    *bytes_read = i;
    return STD_OK;
}

static std_rw_result std_stdout_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err)
{
    (void)desc;
    (void)err;
    uint32_t i = 0;
    for (; i < count && com_putchar_ready(); i++)
        putchar(buf[i]);
    *bytes_written = i;
    return (i < count) ? STD_PENDING : STD_OK;
}

bool std_api_open(void)
{
    char *path = (char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    int fd = -1;
    for (int i = STD_FD_FIRST_FREE; i < STD_FD_MAX; i++)
        if (!std_fd_pool[i].is_open)
        {
            fd = i;
            break;
        }
    if (fd < 0)
        return api_return_errno(API_EMFILE);
    for (size_t i = 0; i < STD_DRIVER_COUNT; i++)
    {
        if (std_drivers[i].handles(path))
        {
            api_errno err = API_EIO;
            int idx = std_drivers[i].open(path, API_A, &err);
            if (idx < 0)
                return api_return_errno(err);
            std_fd_pool[fd].is_open = true;
            std_fd_pool[fd].close = std_drivers[i].close;
            std_fd_pool[fd].read = std_drivers[i].read;
            std_fd_pool[fd].write = std_drivers[i].write;
            std_fd_pool[fd].sync = std_drivers[i].sync;
            std_fd_pool[fd].lseek = std_drivers[i].lseek;
            std_fd_pool[fd].desc = idx;
            return api_return_ax(fd);
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
    api_errno err = API_EIO;
    if (f->close(f->desc, &err) < 0)
        return api_return_errno(err);
    return api_return_ax(0);
}

bool std_api_read_xstack(void)
{
    if (std_fd)
    {
        uint32_t bytes_read;
        api_errno err = API_EIO;
        std_rw_result result = std_fd->read(std_fd->desc,
                                            &std_buf[std_pos], std_size - std_pos,
                                            &bytes_read, &err);
        std_pos += bytes_read;
        if (result == STD_PENDING)
            return api_working();
        std_fd = NULL;
        if (result == STD_ERROR)
            return api_return_errno(err);
        // relocate buffer to top of xstack
        xstack_ptr = XSTACK_SIZE;
        if (std_pos == std_size)
            xstack_ptr -= std_size;
        else
            for (uint16_t i = std_pos; i;)
                xstack[--xstack_ptr] = (uint8_t)std_buf[--i];
        return api_return_ax(std_pos);
    }
    if (!api_pop_uint16_end(&std_size) || std_size > XSTACK_SIZE)
        return api_return_errno(API_EINVAL);
    std_fd_t *fd = std_validate_fd(API_A);
    if (!fd)
        return api_return_errno(API_EBADF);
    if (!fd->read)
        return api_return_errno(API_ENOSYS);
    std_fd = fd;
    std_buf = (char *)&xstack[XSTACK_SIZE - std_size];
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
        uint32_t bytes_read;
        api_errno err = API_EIO;
        std_rw_result result = std_fd->read(std_fd->desc,
                                            &std_buf[std_pos], std_size - std_pos,
                                            &bytes_read, &err);
        std_pos += bytes_read;
        if (result == STD_PENDING)
            return api_working();
        if (result == STD_ERROR)
        {
            std_fd = NULL;
            return api_return_errno(err);
        }
        std_pix = std_pos;
        return api_working();
    }
    uint16_t xram_addr;
    if (!api_pop_uint16(&std_size) || !api_pop_uint16_end(&xram_addr))
        return api_return_errno(API_EINVAL);
    std_fd_t *fd = std_validate_fd(API_A);
    if (!fd)
        return api_return_errno(API_EBADF);
    if (!fd->read)
        return api_return_errno(API_ENOSYS);
    if (std_size > 0x7FFF)
        std_size = 0x7FFF;
    std_buf = (char *)&xram[xram_addr];
    if (std_buf + std_size > (char *)xram + 0x10000)
        return api_return_errno(API_EINVAL);
    std_fd = fd;
    std_pos = 0;
    return api_working();
}

bool std_api_write_xstack(void)
{
    if (std_fd)
    {
        uint32_t bytes_written;
        api_errno err = API_EIO;
        std_rw_result result = std_fd->write(std_fd->desc, &std_buf[std_pos],
                                             std_size - std_pos, &bytes_written, &err);
        std_pos += bytes_written;
        if (result == STD_PENDING)
            return api_working();
        std_fd = NULL;
        if (result == STD_ERROR)
            return api_return_errno(err);
        return api_return_ax(std_pos);
    }
    std_fd_t *fd = std_validate_fd(API_A);
    if (!fd)
        return api_return_errno(API_EBADF);
    if (!fd->write)
        return api_return_errno(API_ENOSYS);
    std_fd = fd;
    std_size = XSTACK_SIZE - xstack_ptr;
    std_buf = (char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    std_pos = 0;
    return api_working();
}

bool std_api_write_xram(void)
{
    if (std_fd)
    {
        uint32_t bytes_written;
        api_errno err = API_EIO;
        std_rw_result result = std_fd->write(std_fd->desc, &std_buf[std_pos],
                                             std_size - std_pos, &bytes_written, &err);
        std_pos += bytes_written;
        if (result == STD_PENDING)
            return api_working();
        std_fd = NULL;
        if (result == STD_ERROR)
            return api_return_errno(err);
        return api_return_ax(std_pos);
    }
    uint16_t xram_addr;
    std_fd_t *fd = std_validate_fd(API_A);
    if (!fd)
        return api_return_errno(API_EBADF);
    if (!fd->write)
        return api_return_errno(API_ENOSYS);
    if (!api_pop_uint16(&std_size) || !api_pop_uint16_end(&xram_addr))
        return api_return_errno(API_EINVAL);
    if (std_size > 0x7FFF)
        std_size = 0x7FFF;
    std_buf = (char *)&xram[xram_addr];
    if (std_buf + std_size > (char *)xram + 0x10000)
        return api_return_errno(API_EINVAL);
    std_fd = fd;
    std_pos = 0;
    return api_working();
}

bool std_api_syncfs(void)
{
    std_fd_t *fd = std_validate_fd(API_A);
    if (!fd)
        return api_return_errno(API_EBADF);
    if (!fd->sync)
        return api_return_errno(API_ENOSYS);
    api_errno err = API_EIO;
    if (fd->sync(fd->desc, &err) < 0)
        return api_return_errno(err);
    return api_return_ax(0);
}

bool std_api_lseek_cc65(void)
{
    int8_t whence_cc65;
    int32_t ofs;
    std_fd_t *fd = std_validate_fd(API_A);
    if (!fd)
        return api_return_errno(API_EBADF);
    if (!api_pop_int8(&whence_cc65) || !api_pop_int32_end(&ofs))
        return api_return_errno(API_EINVAL);
    if (!fd->lseek)
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
    int32_t pos;
    api_errno err = API_EIO;
    if (fd->lseek(fd->desc, whence, ofs, &pos, &err) < 0)
        return api_return_errno(err);
    if (pos < 0)
        return api_return_errno(API_EIO);
    return api_return_axsreg(pos);
}

bool std_api_lseek_llvm(void)
{
    int8_t whence;
    int32_t ofs;
    std_fd_t *fd = std_validate_fd(API_A);
    if (!fd)
        return api_return_errno(API_EBADF);
    if (!api_pop_int8(&whence) || !api_pop_int32_end(&ofs))
        return api_return_errno(API_EINVAL);
    if (!fd->lseek)
        return api_return_errno(API_ENOSYS);
    int32_t pos;
    api_errno err = API_EIO;
    if (fd->lseek(fd->desc, whence, ofs, &pos, &err) < 0)
        return api_return_errno(err);
    if (pos < 0)
        return api_return_errno(API_EIO);
    return api_return_axsreg(pos);
}

void std_run(void)
{
    std_fd = NULL;
    std_pix = -1;
    std_rln_active = false;
    std_rln_needs_nl = false;
    std_rln_pos = 0;
    std_rln_len = 0;
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
        api_errno err;
        std_fd_pool[i].close(std_fd_pool[i].desc, &err);
        std_fd_pool[i].is_open = false;
    }
}
