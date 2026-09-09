/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/driver.h"
#include "core/api/api.h"
#include "core/api/std.h"
#include "core/str/rln.h"
#include "core/str/str.h"
#include "core/sys/com.h"
#include "core/ria/regs.h"
#include "core/sys/xram.h"
#include "core/sys/pix.h"
#include "machine.h"
#include "drivers.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#define STD_FD_MAX 16
#define STD_FD_STDIN 0
#define STD_FD_STDOUT 1
#define STD_FD_STDERR 2
#define STD_FD_CON 3
#define STD_FD_TTY 4
#define STD_FD_FIRST_FREE 5
typedef struct
{
    bool is_open;
    std_rw_result (*close)(int, api_errno *);
    std_rw_result (*read)(int, char *, uint32_t, uint32_t *, api_errno *);
    std_rw_result (*write)(int, const char *, uint32_t, uint32_t *, api_errno *);
    std_rw_result (*sync)(int, api_errno *);
    int (*lseek)(int, int8_t, int32_t, int32_t *, api_errno *);
    int desc;
    /* Which row of std_driver_table opened it. The five pointers above are
     * this build's own addresses, so a savestate writes down this index and
     * rebuilds them from it. */
    uint8_t driver;
} std_fd_t;
static std_fd_t std_fd_pool[STD_FD_MAX];

static std_fd_t *std_fd_active;
static char *std_buf;
static uint16_t std_size;
static uint16_t std_pos;
static uint16_t std_xram_addr;
static uint16_t std_xram_len;

static bool std_rln_active;
static const char *std_rln_buf;
static bool std_rln_needs_nl;
static size_t std_rln_pos;
static size_t std_rln_len;
static bool std_stdin_closed;
static bool std_asked_console;

static std_fd_t *std_validate_fd(int fd)
{
    if (fd < 0 || fd >= STD_FD_MAX || !std_fd_pool[fd].is_open)
        return NULL;
    return &std_fd_pool[fd];
}

static void std_rln_callback(bool timeout, const char *buf)
{
    (void)timeout;
    std_rln_active = false;
    std_rln_buf = buf;
    std_rln_pos = 0;
    std_rln_len = strlen(buf);
    std_rln_needs_nl = true;
}

/* Which of xstack and xram a transfer is landing in. A savestate has to
 * record where the buffer started, because it cannot be worked back out: a
 * read into xram advances std_xram_addr as std_task drains it, and a write
 * into xram never records the address at all. */
#define STD_BUF_NONE 0
#define STD_BUF_XSTACK 1
#define STD_BUF_XRAM 2

void std_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    for (int fd = 0; fd < STD_FD_MAX; fd++)
    {
        std_fd_t *f = &std_fd_pool[fd];
        bool carried = f->is_open && fd >= STD_FD_FIRST_FREE;
        sst_put_bool(c, carried);
        if (!carried)
            continue;
        size_t count;
        const std_driver_t *drivers = std_drivers(&count);
        if (f->driver >= count || !drivers[f->driver].ident)
        {
            sst_fail(c);
            return;
        }
        sst_put_u8(c, f->driver);
        if (!drivers[f->driver].ident(f->desc, c))
        {
            sst_fail(c);
            return;
        }
    }
    uint8_t kind = STD_BUF_NONE;
    uint16_t at = 0;
    if (std_buf)
    {
        if (std_buf >= (char *)xstack && std_buf <= (char *)xstack + XSTACK_SIZE)
        {
            kind = STD_BUF_XSTACK;
            at = (uint16_t)(std_buf - (char *)xstack);
        }
        else
        {
            kind = STD_BUF_XRAM;
            at = (uint16_t)(std_buf - (char *)xram);
        }
    }
    sst_put_u8(c, std_fd_active ? (uint8_t)(std_fd_active - std_fd_pool) : 0xFF);
    sst_put_u8(c, kind);
    sst_put_u16(c, at);
    sst_put_u16(c, std_size);
    sst_put_u16(c, std_pos);
    sst_put_u16(c, std_xram_addr);
    sst_put_u16(c, std_xram_len);
    sst_put_bool(c, std_rln_active);
    sst_put_bool(c, std_rln_needs_nl);
    sst_put_u16(c, (uint16_t)std_rln_pos);
    sst_put_u16(c, (uint16_t)std_rln_len);
    sst_put_bool(c, std_stdin_closed);
    sst_put_bool(c, std_asked_console);
}

bool std_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    size_t count;
    const std_driver_t *drivers = std_drivers(&count);

    /* What this machine has open is closed before the blob's descriptors take
     * their places, because otherwise the host's own file handles leak. The
     * console rows below STD_FD_FIRST_FREE belong to std_init and stay. */
    for (int fd = STD_FD_FIRST_FREE; fd < STD_FD_MAX; fd++)
        if (std_fd_pool[fd].is_open && std_fd_pool[fd].close)
        {
            api_errno ignored;
            while (std_fd_pool[fd].close(std_fd_pool[fd].desc, &ignored) == STD_PENDING)
                ;
            std_fd_pool[fd].is_open = false;
        }

    for (int fd = 0; fd < STD_FD_MAX; fd++)
    {
        bool carried = sst_get_bool(c);
        if (!sst_ok(c))
            return false;
        if (!carried)
            continue;
        uint8_t row = sst_get_u8(c);
        if (!sst_ok(c) || row >= count || !drivers[row].reopen)
            return false;
        api_errno err = API_EIO;
        int desc = drivers[row].reopen(c, &err);
        if (desc < 0 || !sst_ok(c))
            return false;
        std_fd_pool[fd].is_open = true;
        std_fd_pool[fd].close = drivers[row].close;
        std_fd_pool[fd].read = drivers[row].read;
        std_fd_pool[fd].write = drivers[row].write;
        std_fd_pool[fd].sync = drivers[row].sync;
        std_fd_pool[fd].lseek = drivers[row].lseek;
        std_fd_pool[fd].desc = desc;
        std_fd_pool[fd].driver = row;
    }

    uint8_t active = sst_get_u8(c);
    uint8_t kind = sst_get_u8(c);
    uint16_t at = sst_get_u16(c);
    uint16_t size = sst_get_u16(c), pos = sst_get_u16(c);
    uint16_t xaddr = sst_get_u16(c), xlen = sst_get_u16(c);
    bool rln_on = sst_get_bool(c), needs_nl = sst_get_bool(c);
    uint16_t rpos = sst_get_u16(c), rlen = sst_get_u16(c);
    bool closed = sst_get_bool(c), asked = sst_get_bool(c);
    if (!sst_ok(c))
        return false;
    /* An active descriptor must be one that is open, or the first re-dispatch
     * after the load calls through a row whose handlers and desc name
     * nothing. */
    if (active != 0xFF && (active >= STD_FD_MAX || !std_fd_pool[active].is_open))
        return false;
    if (kind > STD_BUF_XRAM)
        return false;
    if (kind == STD_BUF_XSTACK && at > XSTACK_SIZE)
        return false;
    if (kind != STD_BUF_NONE && (uint32_t)at + size > 0x10000)
        return false;
    if (pos > size || rpos > rlen || rlen >= RLN_LINE_MAX)
        return false;

    std_fd_active = (active == 0xFF) ? NULL : &std_fd_pool[active];
    std_buf = (kind == STD_BUF_XSTACK) ? (char *)xstack + at
              : (kind == STD_BUF_XRAM) ? (char *)xram + at
                                       : NULL;
    std_size = size;
    std_pos = pos;
    std_xram_addr = xaddr;
    std_xram_len = xlen;
    std_rln_active = rln_on;
    std_rln_needs_nl = needs_nl;
    std_rln_pos = rpos;
    std_rln_len = rlen;
    std_stdin_closed = closed;
    std_asked_console = asked;
    /* Not carried in the blob. It is either NULL or rln's line buffer, which
     * is at a fixed address, and nothing reads it until a line arrives. */
    std_rln_buf = rln_line();
    return true;
}

rln_read_callback_t std_rln_reader(void)
{
    return std_rln_callback;
}

static std_rw_result std_stdin_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err)
{
    (void)desc;
    (void)err;
    std_asked_console = true;
    *bytes_read = 0;
    if (count == 0)
        return STD_OK;
    if (!std_rln_needs_nl && std_rln_pos >= std_rln_len)
    {
        if (std_stdin_closed)
            return STD_OK;
        if (!std_rln_active)
        {
            std_rln_active = true;
            rln_read_line(std_rln_callback);
        }
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
    *bytes_written = (uint32_t)com_stdout_write(buf, count);
    return (*bytes_written < count) ? STD_PENDING : STD_OK;
}

static std_rw_result std_stderr_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err)
{
    (void)desc;
    (void)err;
    *bytes_written = (uint32_t)com_stderr_write(buf, count);
    return (*bytes_written < count) ? STD_PENDING : STD_OK;
}

static std_rw_result std_con_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err)
{
    std_rw_result result = std_stdin_read(desc, buf, count, bytes_read, err);
    return (result == STD_PENDING) ? STD_OK : result;
}

static std_rw_result std_con_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err)
{
    std_rw_result result = std_stdout_write(desc, buf, count, bytes_written, err);
    return (result == STD_PENDING) ? STD_OK : result;
}

static std_rw_result std_tty_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err)
{
    (void)desc;
    (void)err;
    std_asked_console = true;
    *bytes_read = (uint32_t)com_stdin_read(buf, count);
    return STD_OK;
}

static std_rw_result std_tty_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err)
{
    (void)desc;
    (void)err;
    uint32_t i = 0;
    for (; i < count && com_writable(); i++)
        com_write(buf[i]);
    *bytes_written = i;
    return STD_OK;
}

/* This machine's stdio driver table, listed by its drivers.h. The row order
 * is the order open() tries them, so the filesystem catch-all is last. */
static HOST_IN_FLASH("std_drivers") const std_driver_t std_driver_table[] = {
    RP6502_STD_DRIVERS};

const std_driver_t *std_drivers(size_t *count)
{
    *count = sizeof std_driver_table / sizeof std_driver_table[0];
    return std_driver_table;
}

bool std_api_open(void)
{
    char *path = (char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    if (strcasecmp(path, STR_TTY_COLON) == 0)
        return api_return_ax(STD_FD_TTY);
    if (strcasecmp(path, STR_CON_COLON) == 0)
        return api_return_ax(STD_FD_CON);
    int fd = -1;
    for (int i = STD_FD_FIRST_FREE; i < STD_FD_MAX; i++)
        if (!std_fd_pool[i].is_open)
        {
            fd = i;
            break;
        }
    if (fd < 0)
        return api_return_errno(API_EMFILE);
    size_t count;
    const std_driver_t *drivers = std_drivers(&count);
    for (size_t i = 0; i < count; i++)
    {
        if (drivers[i].handles(path))
        {
            api_errno err = API_EIO;
            int idx = drivers[i].open(path, API_A, &err);
            if (idx < 0)
                return api_return_errno(err);
            std_fd_pool[fd].is_open = true;
            std_fd_pool[fd].close = drivers[i].close;
            std_fd_pool[fd].read = drivers[i].read;
            std_fd_pool[fd].write = drivers[i].write;
            std_fd_pool[fd].sync = drivers[i].sync;
            std_fd_pool[fd].lseek = drivers[i].lseek;
            std_fd_pool[fd].desc = idx;
            std_fd_pool[fd].driver = (uint8_t)i;
            return api_return_ax(fd);
        }
    }
    return api_return_errno(API_ENOENT);
}

bool std_api_close(void)
{
    int fd = API_A;
    if (fd == STD_FD_TTY || fd == STD_FD_CON)
        return api_return_ax(0);
    if (fd < STD_FD_FIRST_FREE || fd >= STD_FD_MAX || !std_fd_pool[fd].is_open)
        return api_return_errno(API_EBADF);
    std_fd_t *f = &std_fd_pool[fd];
    api_errno err = API_EIO;
    std_rw_result result = f->close(f->desc, &err);
    if (result == STD_PENDING)
        return api_working();
    f->is_open = false;
    if (result == STD_ERROR)
        return api_return_errno(err);
    return api_return_ax(0);
}

bool std_api_read_xstack(void)
{
    if (std_fd_active)
    {
        uint32_t bytes_read;
        api_errno err = API_EIO;
        std_rw_result result = std_fd_active->read(std_fd_active->desc,
                                                   &std_buf[std_pos], std_size - std_pos,
                                                   &bytes_read, &err);
        std_pos += bytes_read;
        if (result == STD_PENDING)
            return api_working();
        std_fd_active = NULL;
        if (result == STD_ERROR)
            return api_return_errno(err);
        // A short read leaves the data below the new stack pointer, and the
        // 6502 pops from the top, so it moves up.
        xstack_ptr = XSTACK_SIZE - std_pos;
        if (std_pos != std_size)
            memmove(&xstack[xstack_ptr], std_buf, std_pos);
        return api_return_ax(std_pos);
    }
    if (!api_pop_uint16_end(&std_size) || std_size > XSTACK_SIZE)
        return api_return_errno(API_EINVAL);
    std_fd_t *fd = std_validate_fd(API_A);
    if (!fd)
        return api_return_errno(API_EBADF);
    if (!fd->read)
        return api_return_errno(API_ENOSYS);
    std_fd_active = fd;
    std_buf = (char *)&xstack[XSTACK_SIZE - std_size];
    std_pos = 0;
    return api_working();
}

bool std_api_read_xram(void)
{
    if (std_fd_active)
    {
        if (std_pos < std_size)
        {
            // A chunk of 2048 bytes is tuned for FatFs over USB mass storage
            // with the bulk-only transport.
            uint32_t chunk = std_size - std_pos;
            if (chunk > 2048)
                chunk = 2048;
            uint32_t bytes_read;
            api_errno err = API_EIO;
            std_rw_result result = std_fd_active->read(std_fd_active->desc,
                                                       &std_buf[std_pos], chunk,
                                                       &bytes_read, &err);
            std_pos += bytes_read;
            std_xram_len += bytes_read;
            if (result == STD_PENDING)
                return api_working();
            if (result == STD_ERROR)
            {
                std_xram_len = 0;
                std_fd_active = NULL;
                return api_return_errno(err);
            }
            // A short read is end of file for an xram transfer. It is not
            // for read_xstack, which returns whatever the driver handed back.
            if (bytes_read < chunk)
                std_size = std_pos;
            return api_working();
        }
        // The op does not return until std_task has forwarded every byte
        // read, because a machine with a separate video device keeps its own
        // copy of xram.
        if (std_xram_len > 0)
            return api_working();
        std_fd_active = NULL;
        return api_return_ax(std_pos);
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
    if (xram_addr + std_size > 0x10000)
        std_size = 0x10000 - xram_addr;
    std_buf = (char *)&xram[xram_addr];
    std_fd_active = fd;
    std_pos = 0;
    std_xram_addr = xram_addr;
    std_xram_len = 0;
    return api_working();
}

bool std_api_write_xstack(void)
{
    if (std_fd_active)
    {
        uint32_t bytes_written;
        api_errno err = API_EIO;
        std_rw_result result = std_fd_active->write(std_fd_active->desc, &std_buf[std_pos],
                                                    std_size - std_pos, &bytes_written, &err);
        std_pos += bytes_written;
        if (result == STD_PENDING)
            return api_working();
        std_fd_active = NULL;
        if (result == STD_ERROR)
            return api_return_errno(err);
        return api_return_ax(std_pos);
    }
    std_fd_t *fd = std_validate_fd(API_A);
    if (!fd)
        return api_return_errno(API_EBADF);
    if (!fd->write)
        return api_return_errno(API_ENOSYS);
    std_fd_active = fd;
    std_size = XSTACK_SIZE - xstack_ptr;
    std_buf = (char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    std_pos = 0;
    return api_working();
}

bool std_api_write_xram(void)
{
    if (std_fd_active)
    {
        uint32_t bytes_written;
        api_errno err = API_EIO;
        std_rw_result result = std_fd_active->write(std_fd_active->desc, &std_buf[std_pos],
                                                    std_size - std_pos, &bytes_written, &err);
        std_pos += bytes_written;
        if (result == STD_PENDING)
            return api_working();
        std_fd_active = NULL;
        if (result == STD_ERROR)
            return api_return_errno(err);
        return api_return_ax(std_pos);
    }
    uint16_t xram_addr;
    if (!api_pop_uint16(&std_size) || !api_pop_uint16_end(&xram_addr))
        return api_return_errno(API_EINVAL);
    std_fd_t *fd = std_validate_fd(API_A);
    if (!fd)
        return api_return_errno(API_EBADF);
    if (!fd->write)
        return api_return_errno(API_ENOSYS);
    if (std_size > 0x7FFF)
        std_size = 0x7FFF;
    std_buf = (char *)&xram[xram_addr];
    // A write that runs past the end of xram is refused. A read clamps to
    // what fits instead, which is POSIX read-up-to-N.
    if (std_buf + std_size > (char *)xram + 0x10000)
        return api_return_errno(API_EINVAL);
    std_fd_active = fd;
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
    std_rw_result result = fd->sync(fd->desc, &err);
    if (result == STD_PENDING)
        return api_working();
    if (result == STD_ERROR)
        return api_return_errno(err);
    return api_return_ax(0);
}

static bool std_lseek_common(std_fd_t *fd, int8_t whence, int32_t ofs)
{
    int32_t pos;
    api_errno err = API_EIO;
    if (fd->lseek(fd->desc, whence, ofs, &pos, &err) < 0)
        return api_return_errno(err);
    return api_return_axsreg(pos);
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
    return std_lseek_common(fd, whence, ofs);
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
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)
        return api_return_errno(API_EINVAL);
    return std_lseek_common(fd, whence, ofs);
}

bool std_stdin_waiting(void)
{
    return std_rln_active;
}

bool std_console_asked(void)
{
    return std_asked_console;
}

void std_stdin_eof(void)
{
    std_stdin_closed = true;
    if (!std_rln_active)
        return;
    std_rln_active = false;
    /* A last line that the input never terminated is still a line, so it is
     * handed over before the read is given up; the next read is the one that
     * answers nothing. */
    if (!rln_read_flush())
        rln_read_cancel();
}

void std_task(void)
{
    while (std_xram_len && pix_ready())
    {
        pix_send_xram(std_xram_addr, xram[std_xram_addr]);
        ++std_xram_addr;
        --std_xram_len;
    }
}

void HOST_IN_FLASH("std_init") std_init(void)
{
    std_fd_pool[STD_FD_STDIN].is_open = true;
    std_fd_pool[STD_FD_STDIN].read = std_stdin_read;
    std_fd_pool[STD_FD_STDOUT].is_open = true;
    std_fd_pool[STD_FD_STDOUT].write = std_stdout_write;
    std_fd_pool[STD_FD_STDERR].is_open = true;
    std_fd_pool[STD_FD_STDERR].write = std_stderr_write;
    std_fd_pool[STD_FD_CON].is_open = true;
    std_fd_pool[STD_FD_CON].read = std_con_read;
    std_fd_pool[STD_FD_CON].write = std_con_write;
    std_fd_pool[STD_FD_TTY].is_open = true;
    std_fd_pool[STD_FD_TTY].read = std_tty_read;
    std_fd_pool[STD_FD_TTY].write = std_tty_write;
}

void std_stop(void)
{
    std_fd_active = NULL;
    std_rln_active = false;
    std_rln_needs_nl = false;
    std_rln_pos = 0;
    std_rln_len = 0;
    std_stdin_closed = false;
    std_asked_console = false;
    for (int i = STD_FD_FIRST_FREE; i < STD_FD_MAX; i++)
    {
        if (!std_fd_pool[i].is_open)
            continue;
        api_errno err;
        std_fd_pool[i].close(std_fd_pool[i].desc, &err);
        std_fd_pool[i].is_open = false;
    }
}
