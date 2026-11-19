/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/driver.h"
#include "core/api/api.h"
#include "core/api/std.h"
#include "core/com/console.h"
#include "core/com/tty.h"
#include "core/ria/regs.h"
#include "core/sys/xram.h"
#include "core/sys/pix.h"
#include "osal/fs.h"
#include "machine.h"
#include "drivers.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* As POSIX has them: a descriptor names an open file, and an open file is what
 * a driver opened, with its open mode. dup and dup2 give an open file another
 * descriptor, which shares its position, and the driver's close runs when the
 * last of them is closed. open, dup and dup2 take the lowest free descriptor.
 * Every descriptor can be a file or a console, 0, 1 and 2 included. */
#define STD_FD_MAX 16
#define STD_OPEN_MAX STD_FD_MAX // each open file has a descriptor at least
#define STD_NONE 0xFF
typedef struct
{
    uint8_t refs; // the descriptors that name it, 0 when the slot is free
    /* Which row of std_driver_table opened it. Its handlers are the row's, so
     * a savestate writes down this index and the open mode, and the build
     * that loads it finds its own. */
    uint8_t driver;
    uint8_t mode; // FS_RD and FS_WR
    int desc;     // the driver's own
} std_open_t;
static std_open_t std_opens[STD_OPEN_MAX];
static uint8_t std_fds[STD_FD_MAX]; // 1 + its open file's index in std_opens, 0 when free

/* Files of the filesystem open at once, on every machine, those on SAVE:
 * included: what the Pico's FatFs allows. */
#define STD_FILES_MAX 8

// The open file the op in flight reads, writes, or waits to open.
static std_open_t *std_fd_active;
static char *std_buf;
static uint16_t std_size;
static uint16_t std_pos;
static uint32_t std_xram_addr;          // next byte of a read into xram to forward
static volatile uint32_t std_xram_end;   // xram offset up to which the read has landed
static uint32_t std_xram_lo, std_xram_hi; // the read's xram range while it runs, hi 0 when closed
static int std_xram_err;                  // a failed read's errno, -1 when none

/* A dup2 whose descriptor was the last of an open file whose close is taking
 * more than one pass: the two descriptors, STD_NONE when there is none. */
static uint8_t std_dup2_from = STD_NONE, std_dup2_to = STD_NONE;

static const std_driver_t *std_row(const std_open_t *o)
{
    size_t count;
    return &std_drivers(&count)[o->driver];
}

static std_open_t *std_file(int fd)
{
    if (fd < 0 || fd >= STD_FD_MAX || !std_fds[fd])
        return NULL;
    return &std_opens[std_fds[fd] - 1];
}

/* A direction that the open mode does not include has no handler, so a read
 * or write in that direction gives EBADF. */
static std_rw_result (*std_reader(const std_open_t *o))(int, char *, uint32_t, uint32_t *, api_errno *)
{
    return (o->mode & FS_RD) ? std_row(o)->read : NULL;
}

static std_rw_result (*std_writer(const std_open_t *o))(int, const char *, uint32_t, uint32_t *, api_errno *)
{
    return (o->mode & FS_WR) ? std_row(o)->write : NULL;
}

static int std_lowest_free_fd(void)
{
    for (int fd = 0; fd < STD_FD_MAX; fd++)
        if (!std_fds[fd])
            return fd;
    return -1;
}

/* A close that returns STD_PENDING is called again until it finishes,
 * because no later dispatch repeats it. */
static void std_close_now(std_open_t *o)
{
    api_errno ignored;
    while (std_row(o)->close(o->desc, &ignored) == STD_PENDING)
        ;
    o->refs = 0;
}

// Every open file closed and every descriptor free.
static void std_close_all(void)
{
    for (int i = 0; i < STD_OPEN_MAX; i++)
        if (std_opens[i].refs)
            std_close_now(&std_opens[i]);
    for (int fd = 0; fd < STD_FD_MAX; fd++)
        std_fds[fd] = 0;
}

// A free slot of the pool made the open file of fd.
static std_open_t *std_bind(int fd, uint8_t row, int desc, uint8_t mode)
{
    uint8_t i = 0;
    while (std_opens[i].refs)
        i++;
    std_open_t *o = &std_opens[i];
    o->refs = 1;
    o->driver = row;
    o->mode = mode & (FS_RD | FS_WR);
    o->desc = desc;
    std_fds[fd] = (uint8_t)(i + 1);
    return o;
}

/* fd no longer names its open file. The last descriptor of it closes it, which
 * can take more passes than one: STD_PENDING leaves fd as it was, to be
 * called again. */
static std_rw_result std_release(int fd, api_errno *err)
{
    std_open_t *o = &std_opens[std_fds[fd] - 1];
    if (o->refs == 1)
    {
        std_rw_result result = std_row(o)->close(o->desc, err);
        if (result == STD_PENDING)
            return result;
        o->refs = 0;
        std_fds[fd] = 0;
        return result;
    }
    o->refs--;
    std_fds[fd] = 0;
    return STD_OK;
}

static unsigned std_files_open(void)
{
    unsigned n = 0;
    for (int i = 0; i < STD_OPEN_MAX; i++)
        if (std_opens[i].refs && std_row(&std_opens[i])->file)
            n++;
    return n;
}

bool std_sst_refused(void)
{
    for (int i = 0; i < STD_OPEN_MAX; i++)
    {
        const std_open_t *o = &std_opens[i];
        if (o->refs && std_writer(o) && !std_row(o)->sst.resumes_writes)
            return true;
    }
    return false;
}

/* Which of xstack and xram a transfer is landing in. A savestate has to
 * record where the buffer started, because it cannot be worked back out: a
 * read into xram advances std_xram_addr as std_task drains it, and a write
 * into xram never records the address at all. */
#define STD_BUF_NONE 0
#define STD_BUF_XSTACK 1
#define STD_BUF_XRAM 2

/* A savestate sent to another machine for netplay (SST_SHARED) does not
 * record files opened by path, because that path may not exist on the other
 * machine, and loading the state closes them. A SAVE: file is recorded by its
 * name, which opens the same save on every machine. */
static bool std_left_out(const std_open_t *o, unsigned flags)
{
    return (flags & SST_SHARED) && std_row(o)->open == fs_std_open;
}

static bool std_carried(const std_open_t *o, unsigned flags)
{
    return o->refs && !std_left_out(o, flags);
}

void std_sst_save(sst_cursor_t *c, unsigned flags)
{
    /* Before std_left_out, so a netplay state refuses a writer too rather
     * than leaving it out. */
    if (std_sst_refused())
    {
        sst_fail(c);
        return;
    }
    for (int i = 0; i < STD_OPEN_MAX; i++)
    {
        std_open_t *o = &std_opens[i];
        bool carried = std_carried(o, flags);
        sst_put_bool(c, carried);
        if (!carried)
            continue;
        if (!std_row(o)->sst.ident)
        {
            sst_fail(c);
            return;
        }
        sst_put_u8(c, o->driver);
        sst_put_u8(c, o->mode);
        if (!std_row(o)->sst.ident(o->desc, c))
        {
            sst_fail(c);
            return;
        }
    }
    /* A descriptor of an open file left out is closed by the load. The
     * reference counts are the descriptors, so they are not written down. */
    for (int fd = 0; fd < STD_FD_MAX; fd++)
    {
        uint8_t i = std_fds[fd];
        sst_put_u8(c, i && std_carried(&std_opens[i - 1], flags) ? (uint8_t)(i - 1) : STD_NONE);
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
    /* A read or write in progress on a file that the state does not record
     * is not recorded either, so its op is dispatched again after the load
     * and fails. */
    bool active = std_fd_active && !std_left_out(std_fd_active, flags);
    sst_put_u8(c, active ? (uint8_t)(std_fd_active - std_opens) : STD_NONE);
    /* So is a dup2 whose descriptors are not both recorded. */
    bool dup2 = std_dup2_from != STD_NONE &&
                std_carried(std_file(std_dup2_from), flags) &&
                std_carried(std_file(std_dup2_to), flags);
    sst_put_u8(c, dup2 ? std_dup2_from : STD_NONE);
    sst_put_u8(c, dup2 ? std_dup2_to : STD_NONE);
    sst_put_u8(c, kind);
    sst_put_u16(c, at);
    sst_put_u16(c, std_size);
    sst_put_u16(c, std_pos);
    sst_put_u16(c, (uint16_t)std_xram_addr);
    sst_put_u16(c, (uint16_t)(std_xram_end - std_xram_addr));
}

bool std_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    size_t count;
    const std_driver_t *drivers = std_drivers(&count);

    /* What this machine has open is closed before the blob's open files take
     * their places, because otherwise the host's own file handles leak. A
     * load that fails closes what it reopened, for the same reason. */
    std_close_all();
    bool carried[STD_OPEN_MAX];
    for (int i = 0; i < STD_OPEN_MAX; i++)
    {
        carried[i] = sst_get_bool(c);
        if (!sst_ok(c))
            goto fail;
        if (!carried[i])
            continue;
        uint8_t row = sst_get_u8(c);
        uint8_t mode = sst_get_u8(c);
        if (!sst_ok(c) || row >= count || !drivers[row].sst.reopen || !(mode & (FS_RD | FS_WR)) ||
            (mode & ~(FS_RD | FS_WR)))
            goto fail;
        api_errno err = API_EIO;
        int desc = drivers[row].sst.reopen(c, mode, &err);
        if (desc < 0)
            goto fail;
        // Held by the load until the descriptors are counted.
        std_opens[i] = (std_open_t){.refs = 1, .driver = row, .mode = mode, .desc = desc};
        if (!sst_ok(c))
            goto fail;
    }
    uint8_t named[STD_OPEN_MAX] = {0};
    for (int fd = 0; fd < STD_FD_MAX; fd++)
    {
        uint8_t i = sst_get_u8(c);
        if (!sst_ok(c) || (i != STD_NONE && (i >= STD_OPEN_MAX || !carried[i])))
            goto fail;
        if (i == STD_NONE)
            continue;
        std_fds[fd] = (uint8_t)(i + 1);
        named[i]++;
    }
    /* An open file that no descriptor names would never be closed. */
    for (int i = 0; i < STD_OPEN_MAX; i++)
        if (carried[i])
        {
            if (!named[i])
                std_close_now(&std_opens[i]);
            std_opens[i].refs = named[i];
        }

    uint8_t active = sst_get_u8(c);
    uint8_t from = sst_get_u8(c), to = sst_get_u8(c);
    uint8_t kind = sst_get_u8(c);
    uint16_t at = sst_get_u16(c);
    uint16_t size = sst_get_u16(c), pos = sst_get_u16(c);
    uint16_t xaddr = sst_get_u16(c), xlen = sst_get_u16(c);
    if (!sst_ok(c))
        goto fail;
    /* An active open file must be one that is open, or the first re-dispatch
     * after the load calls through a row whose desc names nothing. A dup2
     * in flight is between two open descriptors that differ. */
    if (active != STD_NONE && (active >= STD_OPEN_MAX || !std_opens[active].refs))
        goto fail;
    if ((from == STD_NONE) != (to == STD_NONE) ||
        (from != STD_NONE && (from >= STD_FD_MAX || to >= STD_FD_MAX || from == to ||
                              !std_fds[from] || !std_fds[to])))
        goto fail;
    if (kind > STD_BUF_XRAM)
        goto fail;
    if (kind == STD_BUF_XSTACK && at > XSTACK_SIZE)
        goto fail;
    if (kind != STD_BUF_NONE && (uint32_t)at + size > 0x10000)
        goto fail;
    if (pos > size)
        goto fail;

    std_fd_active = (active == STD_NONE) ? NULL : &std_opens[active];
    std_dup2_from = from;
    std_dup2_to = to;
    std_buf = (kind == STD_BUF_XSTACK) ? (char *)xstack + at
              : (kind == STD_BUF_XRAM) ? (char *)xram + at
                                       : NULL;
    std_size = size;
    std_pos = pos;
    std_xram_addr = xaddr;
    std_xram_end = (uint32_t)xaddr + xlen;
    std_xram_lo = (kind == STD_BUF_XRAM) ? at : 0;
    std_xram_hi = (kind == STD_BUF_XRAM) ? (uint32_t)at + size : 0;
    /* Not carried. A machine that saves streams nothing, so a failed read
     * returns at once. */
    std_xram_err = -1;
    return true;
fail:
    std_close_all();
    return false;
}

/* This machine's stdio driver table: one row for each driver in its
 * drivers.h's RP6502_STD_DRIVERS, made of that driver's STD and STD_SST. The
 * row order is the order open() tries them, so the filesystem catch-all is
 * last. STD_SST is kept only on a machine whose drivers.h says it makes
 * savestates, so on one that does not, nothing links what it names. The list
 * is expanded as it stands rather than through DRIVERS_FORWARD, because the
 * rows' commas would split it. */
static const std_driver_t std_driver_table[] = {
#define DRIVER(row) {row}
#define STD(...) __VA_ARGS__,
#ifdef RP6502_SST
#define STD_SST(...) .sst = {__VA_ARGS__},
#endif
#include "core/sys/driver_walk.h"
    RP6502_STD_DRIVERS
#include "core/sys/driver_walk_end.h"
};

const std_driver_t *std_drivers(size_t *count)
{
    *count = sizeof std_driver_table / sizeof std_driver_table[0];
    return std_driver_table;
}

// The row that claims path, or -1 when none does.
static int std_row_for(const char *path)
{
    for (size_t i = 0; i < sizeof std_driver_table / sizeof std_driver_table[0]; i++)
        if (std_driver_table[i].handles(path))
            return (int)i;
    return -1;
}

bool std_api_open(void)
{
    api_errno err = API_EIO;
    if (std_fd_active)
    {
        std_open_t *o = std_fd_active;
        std_rw_result result = std_row(o)->opening(o->desc, &err);
        if (result == STD_PENDING)
            return api_working();
        std_fd_active = NULL;
        // A file being opened has the one descriptor.
        int fd = 0;
        while (std_fds[fd] != (uint8_t)(o - std_opens + 1))
            fd++;
        if (result == STD_ERROR)
        {
            std_close_now(o);
            std_fds[fd] = 0;
            return api_return_errno(err);
        }
        return api_return_ax((uint16_t)fd);
    }
    const char *path = api_pop_path_end(&err);
    if (!(API_A & (FS_RD | FS_WR)))
        return api_return_errno(API_EINVAL);
    if (!path)
        return api_return_errno(err);
    int fd = std_lowest_free_fd();
    if (fd < 0)
        return api_return_errno(API_EMFILE);
    int i = std_row_for(path);
    if (i < 0)
        return api_return_errno(API_ENOENT);
    if ((API_A & FS_WR) && !std_driver_table[i].write)
        return api_return_errno(API_EACCES);
    if (std_driver_table[i].file && std_files_open() >= STD_FILES_MAX)
        return api_return_errno(API_EMFILE);
    err = API_EIO;
    int idx = std_driver_table[i].open(path, API_A, &err);
    if (idx < 0)
        return api_return_errno(err);
    std_open_t *o = std_bind(fd, (uint8_t)i, idx, API_A);
    if (!std_driver_table[i].opening)
        return api_return_ax((uint16_t)fd);
    std_fd_active = o;
    return api_working();
}

bool std_api_close(void)
{
    int fd = API_A;
    if (!std_file(fd))
        return api_return_errno(API_EBADF);
    api_errno err = API_EIO;
    std_rw_result result = std_release(fd, &err);
    if (result == STD_PENDING)
        return api_working();
    if (result == STD_ERROR)
        return api_return_errno(err);
    return api_return_ax(0);
}

// fd names the open file that names.
static void std_attach(int fd, int names)
{
    std_fds[fd] = std_fds[names];
    std_opens[std_fds[fd] - 1].refs++;
}

bool std_api_dup(void)
{
    if (!std_file(API_A))
        return api_return_errno(API_EBADF);
    int fd = std_lowest_free_fd();
    if (fd < 0)
        return api_return_errno(API_EMFILE);
    std_attach(fd, API_A);
    return api_return_ax((uint16_t)fd);
}

/* 1 for a descriptor of the console, CON: or TTY:, and 0 for anything else
 * open, which is how a library tells a terminal from a redirected file. */
bool std_api_isatty(void)
{
    std_open_t *o = std_file(API_A);
    if (!o)
        return api_return_errno(API_EBADF);
    return api_return_ax(std_row(o)->terminal);
}

bool std_exec_refused(void)
{
    for (int i = 0; i < STD_OPEN_MAX; i++)
        if (std_opens[i].refs && std_row(&std_opens[i])->refuses_exec)
            return true;
    return false;
}

/* fildes2 is in A and fildes on the xstack, the last argument in A as for
 * every call. An open fildes2 is closed first, and as POSIX has it, an error
 * of that close is not reported. */
bool std_api_dup2(void)
{
    if (std_dup2_from == STD_NONE)
    {
        int16_t from;
        int to = API_A;
        if (!api_pop_int16_end(&from))
            return api_return_errno(API_EINVAL);
        if (!std_file(from) || to >= STD_FD_MAX)
            return api_return_errno(API_EBADF);
        if (to == from)
            return api_return_ax((uint16_t)to);
        if (!std_fds[to])
        {
            std_attach(to, from);
            return api_return_ax((uint16_t)to);
        }
        std_dup2_from = (uint8_t)from;
        std_dup2_to = (uint8_t)to;
    }
    api_errno ignored;
    if (std_release(std_dup2_to, &ignored) == STD_PENDING)
        return api_working();
    std_attach(std_dup2_to, std_dup2_from);
    uint8_t to = std_dup2_to;
    std_dup2_from = std_dup2_to = STD_NONE;
    return api_return_ax(to);
}

bool std_api_read_xstack(void)
{
    if (std_fd_active)
    {
        uint32_t bytes_read;
        api_errno err = API_EIO;
        std_rw_result result = std_reader(std_fd_active)(std_fd_active->desc,
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
    std_open_t *fd = std_file(API_A);
    if (!fd || !std_reader(fd))
        return api_return_errno(API_EBADF);
    std_fd_active = fd;
    std_buf = (char *)&xstack[XSTACK_SIZE - std_size];
    std_pos = 0;
    return api_working();
}

void std_xram_landed(const void *dst, uint32_t n)
{
    uintptr_t off = (uintptr_t)dst - (uintptr_t)xram;
    if (off >= std_xram_lo && off < std_xram_hi && n <= std_xram_hi - off &&
        off + n > std_xram_end)
        std_xram_end = off + n;
}

bool std_api_read_xram(void)
{
    if (std_fd_active)
    {
        if (std_pos < std_size)
        {
            uint32_t want = std_size - std_pos;
            uint32_t bytes_read;
            api_errno err = API_EIO;
            std_rw_result result = std_reader(std_fd_active)(std_fd_active->desc,
                                                             &std_buf[std_pos], want,
                                                             &bytes_read, &err);
            std_pos += bytes_read;
            std_xram_end = std_xram_lo + std_pos;
            if (result == STD_PENDING)
                return api_working();
            if (result == STD_OK)
            {
                // A short read is end of file for an xram transfer. It is
                // not for read_xstack, which returns whatever the driver
                // handed back.
                if (bytes_read < want)
                    std_size = std_pos;
                return api_working();
            }
            // A failed read drops what it has not streamed and returns its
            // errno once the rest is out.
            std_xram_end = std_xram_addr;
            std_xram_hi = 0;
            std_size = std_pos;
            std_xram_err = err;
        }
        // The op does not return until every byte read is queued ahead of
        // anything the 6502 sends next, because a machine with a separate
        // video device keeps its own copy of xram.
        if (std_xram_addr < std_xram_end || !pix_stream_done())
            return api_working();
        std_xram_hi = 0;
        std_fd_active = NULL;
        if (std_xram_err >= 0)
            return api_return_errno(std_xram_err);
        return api_return_ax(std_pos);
    }
    uint16_t xram_addr;
    if (!api_pop_uint16(&std_size) || !api_pop_uint16_end(&xram_addr))
        return api_return_errno(API_EINVAL);
    std_open_t *fd = std_file(API_A);
    if (!fd || !std_reader(fd))
        return api_return_errno(API_EBADF);
    if (std_size > 0x7FFF)
        std_size = 0x7FFF;
    if (xram_addr + std_size > 0x10000)
        std_size = 0x10000 - xram_addr;
    std_buf = (char *)&xram[xram_addr];
    std_fd_active = fd;
    std_pos = 0;
    std_xram_addr = xram_addr;
    std_xram_end = xram_addr;
    std_xram_lo = xram_addr;
    std_xram_hi = (uint32_t)xram_addr + std_size;
    std_xram_err = -1;
    return api_working();
}

bool std_api_write_xstack(void)
{
    if (std_fd_active)
    {
        uint32_t bytes_written;
        api_errno err = API_EIO;
        std_rw_result result = std_writer(std_fd_active)(std_fd_active->desc, &std_buf[std_pos],
                                                    std_size - std_pos, &bytes_written, &err);
        std_pos += bytes_written;
        if (result == STD_PENDING)
            return api_working();
        std_fd_active = NULL;
        if (result == STD_ERROR)
            return api_return_errno(err);
        return api_return_ax(std_pos);
    }
    std_open_t *fd = std_file(API_A);
    if (!fd || !std_writer(fd))
        return api_return_errno(API_EBADF);
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
        std_rw_result result = std_writer(std_fd_active)(std_fd_active->desc, &std_buf[std_pos],
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
    std_open_t *fd = std_file(API_A);
    if (!fd || !std_writer(fd))
        return api_return_errno(API_EBADF);
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
    std_open_t *fd = std_file(API_A);
    if (!fd)
        return api_return_errno(API_EBADF);
    if (!std_row(fd)->sync)
        return api_return_errno(API_EINVAL);
    api_errno err = API_EIO;
    std_rw_result result = std_row(fd)->sync(fd->desc, &err);
    if (result == STD_PENDING)
        return api_working();
    if (result == STD_ERROR)
        return api_return_errno(err);
    return api_return_ax(0);
}

static bool std_lseek_common(std_open_t *fd, int8_t whence, int32_t ofs)
{
    int32_t pos;
    api_errno err = API_EIO;
    if (std_row(fd)->lseek(fd->desc, whence, ofs, &pos, &err) < 0)
        return api_return_errno(err);
    return api_return_axsreg(pos);
}

bool std_api_lseek_cc65(void)
{
    int8_t whence_cc65;
    int32_t ofs;
    std_open_t *fd = std_file(API_A);
    if (!fd)
        return api_return_errno(API_EBADF);
    if (!api_pop_int8(&whence_cc65) || !api_pop_int32_end(&ofs))
        return api_return_errno(API_EINVAL);
    if (!std_row(fd)->lseek)
        return api_return_errno(API_ESPIPE);
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
    std_open_t *fd = std_file(API_A);
    if (!fd)
        return api_return_errno(API_EBADF);
    if (!api_pop_int8(&whence) || !api_pop_int32_end(&ofs))
        return api_return_errno(API_EINVAL);
    if (!std_row(fd)->lseek)
        return api_return_errno(API_ESPIPE);
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)
        return api_return_errno(API_EINVAL);
    return std_lseek_common(fd, whence, ofs);
}

void HOST_IN_RAM_FUNC std_task(void)
{
    if (std_xram_addr < std_xram_end)
        std_xram_addr = pix_stream_xram(std_xram_addr, std_xram_end);
}

void std_reset(void)
{
    const char *const console[] = {"CON:0", "CON:1", "CON:2"};
    std_close_all();
    for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; fd++)
    {
        int row = std_row_for(console[fd]);
        api_errno err;
        int desc = std_driver_table[row].open(console[fd], FS_RD | FS_WR, &err);
        std_bind(fd, (uint8_t)row, desc, FS_RD | FS_WR);
    }
}

void HOST_IN_FLASH_FUNC std_init(void)
{
    std_reset();
}

/* A stop ends what the program had in flight. Its descriptors stay, for the
 * program an exec starts; the end of the chain is where std_reset puts the
 * console back. */
void HOST_IN_FLASH_FUNC std_stop(void)
{
    std_fd_active = NULL;
    std_dup2_from = std_dup2_to = STD_NONE;
    std_xram_hi = 0;
    std_xram_end = std_xram_addr;
}
