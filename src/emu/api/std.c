/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The 6502 stdio/file syscalls (ria/api/std.c ops 0x14-0x1E). This is the stdio
 * OPEN DISPATCHER: a path is claimed by the first driver in std_drivers[] whose
 * handles() returns true — ROM: assets first, then the RAM FatFs (only while
 * --tmpdrive is mounted), then the native host filesystem as the always-true
 * catch-all, last. This platform's own static table, gated by handles(), mirrors
 * the firmware's ria/api/std.c; the drivers share the same std_driver_t ABI (int
 * descriptor, std_rw_result, api_errno). open() caches the driver's ops in the fd
 * pool so later ops dispatch without re-parsing the path.
 *
 * Two things differ from the firmware:
 *   - A file read is issued in one std_read call, never chunked/PIX-drained over
 *     many ticks. In the windowed build both MSC0: host I/O and ROM: asset reads
 *     run async (POSIX AIO), re-polled until the aiocb completes (STD_PENDING);
 *     headless they finish synchronously. stdin re-dispatches until a line is ready.
 *   - stdin is the one blocking call: with no line ready it returns api_working()
 *     and the machine re-dispatches it each frame while rln_task() drains the
 *     keyboard, exactly as the hardware polls the RIA.
 *
 * fd 0-4 are the reserved console streams (stdin/stdout/stderr/con/tty); fd 5-15
 * are open files. Console writes go to the terminal via emu_stdout_write; con/
 * stdin reads come from the line editor.
 */

#include "emu/api/api.h"
#include "emu/api/std.h"
#include "emu/host/host.h"
#include "emu/mon/install.h"
#include "emu/mon/rom.h"
#include "emu/host/dir.h"
#include "emu/host/fs.h"
#include "emu/sys/mem.h"
#include "emu/usb/msc.h"
#include "api/api.h"
#include "api/dir.h"
#include "api/std.h"
#include "api/fat.h"
#include "aud/bel.h"
#include "str/rln.h"
#include "sys/com.h"
#include <stdio.h> /* SEEK_SET/SEEK_CUR/SEEK_END */
#include <string.h>
#include <strings.h> /* strcasecmp */

#define STD_FD_STDIN 0
#define STD_FD_STDOUT 1
#define STD_FD_STDERR 2
#define STD_FD_CON 3
#define STD_FD_TTY 4
#define STD_FD_FIRST_FREE 5
#define STD_FD_MAX 16

/* This platform's stdio driver table (its own vtable, never mutated): an inactive
 * filesystem is turned off by handles() alone. The RAM FatFs (the shared fat_std_*
 * driver) claims MSC0: only while --tmpdrive is mounted; otherwise the native host
 * filesystem — the always-true catch-all, listed last — reclaims it. */
static bool emu_fat_handles(const char *path)
{
    (void)path;
    return emu_fat_active();
}
static const std_driver_t std_drivers[] = {
    {rom_std_handles, rom_std_open, rom_std_close, rom_std_read, NULL, NULL, rom_std_lseek},
    {emu_fat_handles, fat_std_open, fat_std_close, fat_std_read, fat_std_write, fat_std_sync, fat_std_lseek},
    {host_std_handles, host_std_open, host_std_close, host_std_read, host_std_write, host_std_sync, host_std_lseek},
};
#define STD_DRIVER_COUNT (sizeof(std_drivers) / sizeof(std_drivers[0]))

/* The stdio file descriptor pool. The console streams set the ops directly;
 * open()ed files copy them from the matching driver. */
typedef struct
{
    bool is_open;
    std_rw_result (*close)(int, api_errno *);
    std_rw_result (*read)(int, char *, uint32_t, uint32_t *, api_errno *);
    std_rw_result (*write)(int, const char *, uint32_t, uint32_t *, api_errno *);
    std_rw_result (*sync)(int, api_errno *);
    int (*lseek)(int, int8_t, int32_t, int32_t *, api_errno *);
    int desc;
} std_fd_t;
static std_fd_t std_fd_pool[STD_FD_MAX];

static std_fd_t *std_validate_fd(int fd)
{
    if (fd < 0 || fd >= STD_FD_MAX || !std_fd_pool[fd].is_open)
        return NULL;
    return &std_fd_pool[fd];
}

/* ------------------------------------------------------------------ */
/* Console streams: stdin via the line editor, writes to the terminal  */
/* ------------------------------------------------------------------ */

static bool std_rln_active;
static const char *std_rln_buf;
static bool std_rln_needs_nl;
static size_t std_rln_pos;
static size_t std_rln_len;

static void std_rln_callback(bool timeout, const char *buf)
{
    (void)timeout;
    std_rln_active = false;
    std_rln_buf = buf;
    std_rln_pos = 0;
    std_rln_len = strlen(buf);
    std_rln_needs_nl = true;
}

/* stdin/con read (ports the firmware std_stdin_read): STD_PENDING with no line
 * ready (a request is queued; the machine re-dispatches us), STD_OK once bytes
 * (and the trailing newline) are delivered. */
static std_rw_result std_stdin_read(int desc, char *buf, uint32_t count, uint32_t *got, api_errno *err)
{
    (void)desc, (void)err;
    *got = 0;
    if (!std_rln_needs_nl && std_rln_pos >= std_rln_len)
    {
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
    *got = i;
    return STD_OK;
}

/* CON: read (ports the firmware std_con_read): non-blocking — no line ready reads
 * 0 bytes rather than spinning the 6502 (unlike stdin). */
static std_rw_result std_con_read(int desc, char *buf, uint32_t count, uint32_t *got, api_errno *err)
{
    std_rw_result r = std_stdin_read(desc, buf, count, got, err);
    return (r == STD_PENDING) ? STD_OK : r;
}

/* TTY: read (ports the firmware std_tty_read): raw, non-blocking drain of queued
 * keystroke bytes — no cooking/echo, *got=0 when idle. */
static std_rw_result std_tty_read(int desc, char *buf, uint32_t count, uint32_t *got, api_errno *err)
{
    (void)desc, (void)err;
    uint32_t i = 0;
    for (; i < count; i++)
    {
        com_source_t src = COM_SOURCE_KBD;
        int ch = com_getchar(&src);
        if (ch < 0)
            break;
        buf[i] = (char)ch;
    }
    *got = i;
    return STD_OK;
}

/* Ring the teletype bell on any BEL (0x07) in a program's console output, like
 * the firmware's com TX scan (ria/sys/com.c). The byte still passes through to
 * the terminal; this rings only on program output, not the rln line echo. */
static void std_bell_scan(const char *buf, uint32_t n)
{
    if (!com_get_bel())
        return;
    for (uint32_t i = 0; i < n; i++)
        if (buf[i] == '\a')
            bel_add(&bel_teletype);
}

/* stdout/stderr/con/tty write: to the terminal, instantly (no drain). Unlike the
 * firmware's separate std_stdout_write/std_con_write/std_tty_write, the emulator
 * has one write path (no UART TX ring), so every write slot uses this. */
static std_rw_result std_stdout_write(int desc, const char *buf, uint32_t count, uint32_t *put, api_errno *err)
{
    (void)desc, (void)err;
    std_bell_scan(buf, count);
    emu_stdout_write(buf, count);
    *put = count;
    return STD_OK;
}

static void setup_console(void)
{
    memset(std_fd_pool, 0, sizeof(std_fd_pool));
    std_fd_pool[STD_FD_STDIN] = (std_fd_t){.is_open = true, .read = std_stdin_read};
    std_fd_pool[STD_FD_STDOUT] = (std_fd_t){.is_open = true, .write = std_stdout_write};
    std_fd_pool[STD_FD_STDERR] = (std_fd_t){.is_open = true, .write = std_stdout_write};
    std_fd_pool[STD_FD_CON] = (std_fd_t){.is_open = true, .read = std_con_read, .write = std_stdout_write};
    std_fd_pool[STD_FD_TTY] = (std_fd_t){.is_open = true, .read = std_tty_read, .write = std_stdout_write};
}

/* ------------------------------------------------------------------ */
/* Host-side file API over the driver table (the 6502 reaches it via    */
/* the std_api_* syscalls below; main.c/tests use it directly)          */
/* ------------------------------------------------------------------ */

int std_open(const char *path, uint8_t flags, api_errno *err)
{
    if (strcasecmp(path, "CON:") == 0)
        return STD_FD_CON;
    if (strcasecmp(path, "TTY:") == 0)
        return STD_FD_TTY;
    int fd = -1;
    for (int i = STD_FD_FIRST_FREE; i < STD_FD_MAX; i++)
        if (!std_fd_pool[i].is_open)
        {
            fd = i;
            break;
        }
    if (fd < 0)
    {
        if (err)
            *err = API_EMFILE;
        return -1;
    }
    for (size_t i = 0; i < STD_DRIVER_COUNT; i++)
    {
        if (!std_drivers[i].handles(path))
            continue;
        api_errno e = API_EIO;
        int desc = std_drivers[i].open(path, flags, &e);
        if (desc < 0)
        {
            if (err)
                *err = e;
            return -1;
        }
        std_fd_pool[fd].is_open = true;
        std_fd_pool[fd].desc = desc;
        std_fd_pool[fd].close = std_drivers[i].close;
        std_fd_pool[fd].read = std_drivers[i].read;
        std_fd_pool[fd].write = std_drivers[i].write;
        std_fd_pool[fd].sync = std_drivers[i].sync;
        std_fd_pool[fd].lseek = std_drivers[i].lseek;
        return fd;
    }
    if (err)
        *err = API_ENOENT; /* unreachable: the host driver is a catch-all */
    return -1;
}

bool std_writable(int fd)
{
    return fd >= 0 && fd < STD_FD_MAX && std_fd_pool[fd].is_open && std_fd_pool[fd].write != NULL;
}

std_rw_result std_read(int fd, char *buf, uint32_t n, uint32_t *got, api_errno *err)
{
    *got = 0;
    if (fd < 0 || fd >= STD_FD_MAX || !std_fd_pool[fd].is_open || !std_fd_pool[fd].read)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    return std_fd_pool[fd].read(std_fd_pool[fd].desc, buf, n, got, err);
}

std_rw_result std_write(int fd, const char *buf, uint32_t n, uint32_t *put, api_errno *err)
{
    *put = 0;
    if (fd < 0 || fd >= STD_FD_MAX || !std_fd_pool[fd].is_open || !std_fd_pool[fd].write)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    return std_fd_pool[fd].write(std_fd_pool[fd].desc, buf, n, put, err);
}

long std_lseek(int fd, long offset, int whence)
{
    if (fd < 0 || fd >= STD_FD_MAX || !std_fd_pool[fd].is_open || !std_fd_pool[fd].lseek)
        return -1;
    int32_t pos;
    api_errno err;
    if (std_fd_pool[fd].lseek(std_fd_pool[fd].desc, (int8_t)whence, (int32_t)offset, &pos, &err) < 0)
        return -1;
    return pos;
}

void std_close(int fd)
{
    if (fd < STD_FD_FIRST_FREE || fd >= STD_FD_MAX || !std_fd_pool[fd].is_open)
        return; /* console streams (0-4) stay open */
    if (std_fd_pool[fd].close)
    {
        api_errno err;
        std_fd_pool[fd].close(std_fd_pool[fd].desc, &err);
    }
    memset(&std_fd_pool[fd], 0, sizeof(std_fd_pool[fd]));
}

/* Close every open file fd (the driver close frees its descriptor); machine
 * reset. The console streams are re-established by setup_console (std_reset). */
void std_files_reset(void)
{
    for (int fd = STD_FD_FIRST_FREE; fd < STD_FD_MAX; fd++)
        std_close(fd);
}

/* ------------------------------------------------------------------ */
/* open / close                                                        */
/* ------------------------------------------------------------------ */

bool std_api_open(void)
{
    const char *path = (const char *)&xstack[xstack_ptr];
    uint8_t flags = API_A;
    xstack_ptr = XSTACK_SIZE;
    api_errno err = API_EIO;
    int fd = std_open(path, flags, &err);
    if (fd < 0)
        return api_return_errno(err);
    return api_return_ax((uint16_t)fd);
}

bool std_api_close(void)
{
    int fd = API_A;
    if (fd == STD_FD_CON || fd == STD_FD_TTY)
        return api_return_ax(0); /* CON:/TTY: stay open; 0/1/2 -> EBADF */
    if (fd < STD_FD_FIRST_FREE || fd >= STD_FD_MAX || !std_fd_pool[fd].is_open)
        return api_return_errno(API_EBADF);
    std_close(fd);
    return api_return_ax(0);
}

/* ------------------------------------------------------------------ */
/* read                                                                */
/* ------------------------------------------------------------------ */

/* In-flight read (the polling I/O model). Only one read is ever in flight (the
 * 6502 is blocked on it), so one state serves both the xstack and xram paths. A
 * poll reads what is available now, advancing rd.got; STD_PENDING means poll again
 * (stdin until a line; an async MSC0:/ROM: read until its aiocb completes). */
static struct
{
    bool active;
    bool xram;     /* dest is xram[addr] (read_xram) vs rd.buf (read_xstack) */
    int fd;
    char *buf;     /* xstack dest (when !xram) */
    uint16_t addr; /* xram dest base (when xram) */
    uint16_t size;
    uint16_t got;
    api_errno err;
} rd;

static std_rw_result rd_poll(void)
{
    char *dst = rd.xram ? (char *)&xram[rd.addr + rd.got] : rd.buf + rd.got;
    uint32_t got = 0;
    std_rw_result r = std_read(rd.fd, dst, (uint32_t)(rd.size - rd.got), &got, &rd.err);
    rd.got += (uint16_t)got;
    return r;
}

bool std_api_read_xstack(void)
{
    if (!rd.active)
    {
        uint16_t size;
        if (!api_pop_uint16_end(&size) || size > XSTACK_SIZE)
            return api_return_errno(API_EINVAL);
        std_fd_t *fd = std_validate_fd(API_A);
        if (!fd)
            return api_return_errno(API_EBADF);
        if (!fd->read)
            return api_return_errno(API_ENOSYS);
        rd.active = true;
        rd.xram = false;
        rd.fd = API_A;
        rd.buf = (char *)&xstack[XSTACK_SIZE - size];
        rd.size = size;
        rd.got = 0;
    }
    std_rw_result r = rd_poll();
    if (r == STD_PENDING)
        return api_working();
    rd.active = false;
    if (r == STD_ERROR)
        return api_return_errno(rd.err);
    xstack_ptr = XSTACK_SIZE - rd.got;
    if (rd.got != rd.size)
        memmove(&xstack[xstack_ptr], rd.buf, rd.got);
    return api_return_ax(rd.got);
}

bool std_api_read_xram(void)
{
    if (!rd.active)
    {
        uint16_t size, xram_addr;
        if (!api_pop_uint16(&size) || !api_pop_uint16_end(&xram_addr))
            return api_return_errno(API_EINVAL);
        std_fd_t *fd = std_validate_fd(API_A);
        if (!fd)
            return api_return_errno(API_EBADF);
        if (!fd->read)
            return api_return_errno(API_ENOSYS);
        if (size > 0x7FFF)
            size = 0x7FFF;
        if ((int)xram_addr + size > 0x10000)
            size = (uint16_t)(0x10000 - xram_addr);
        rd.active = true;
        rd.xram = true;
        rd.fd = API_A;
        rd.addr = xram_addr;
        rd.size = size;
        rd.got = 0;
    }
    std_rw_result r = rd_poll();
    if (r == STD_PENDING)
        return api_working();
    rd.active = false;
    if (r == STD_ERROR)
        return api_return_errno(rd.err);
    return api_return_ax(rd.got);
}

/* ------------------------------------------------------------------ */
/* write                                                               */
/* ------------------------------------------------------------------ */

/* In-flight write (mirrors rd). The driver submits the transfer on the first
 * poll and returns STD_PENDING until it completes (async window); the source
 * (xstack/xram) stays put while the 6502 spins, so the pointer is kept. */
static struct
{
    bool active;
    int fd;
    const char *buf;
    uint16_t size;
    api_errno err;
} wr;

static bool wr_finish(void)
{
    uint32_t put = 0;
    std_rw_result r = std_fd_pool[wr.fd].write(std_fd_pool[wr.fd].desc, wr.buf, wr.size, &put, &wr.err);
    if (r == STD_PENDING)
        return api_working();
    wr.active = false;
    if (r != STD_OK)
        return api_return_errno(wr.err);
    return api_return_ax((uint16_t)put);
}

bool std_api_write_xstack(void)
{
    if (!wr.active)
    {
        uint16_t size = (uint16_t)(XSTACK_SIZE - xstack_ptr);
        const char *buf = (const char *)&xstack[xstack_ptr];
        xstack_ptr = XSTACK_SIZE;
        std_fd_t *fd = std_validate_fd(API_A);
        if (!fd)
            return api_return_errno(API_EBADF);
        if (!fd->write)
            return api_return_errno(API_ENOSYS);
        wr.active = true;
        wr.fd = API_A;
        wr.buf = buf;
        wr.size = size;
    }
    return wr_finish();
}

bool std_api_write_xram(void)
{
    if (!wr.active)
    {
        uint16_t size, xram_addr;
        if (!api_pop_uint16(&size) || !api_pop_uint16_end(&xram_addr))
            return api_return_errno(API_EINVAL);
        std_fd_t *fd = std_validate_fd(API_A);
        if (size > 0x7FFF)
            size = 0x7FFF;
        if ((int)xram_addr + size > 0x10000)
            return api_return_errno(API_EINVAL);
        if (!fd)
            return api_return_errno(API_EBADF);
        if (!fd->write)
            return api_return_errno(API_ENOSYS);
        wr.active = true;
        wr.fd = API_A;
        wr.buf = (const char *)&xram[xram_addr];
        wr.size = size;
    }
    return wr_finish();
}

/* ------------------------------------------------------------------ */
/* lseek / syncfs                                                      */
/* ------------------------------------------------------------------ */

/* Console streams (fd 0-4) are valid fds but have no lseek/sync (those slots are
 * NULL): ENOSYS, like the firmware. A closed/out-of-range fd is EBADF. */

static bool std_lseek_common(std_fd_t *fd, int8_t whence, int32_t ofs)
{
    int32_t pos;
    api_errno err = API_EIO;
    if (fd->lseek(fd->desc, whence, ofs, &pos, &err) < 0)
        return api_return_errno(err);
    return api_return_axsreg((uint32_t)pos);
}

bool std_api_lseek_cc65(void)
{
    std_fd_t *fd = std_validate_fd(API_A);
    if (!fd)
        return api_return_errno(API_EBADF);
    int8_t whence_cc65;
    int32_t ofs;
    if (!api_pop_int8(&whence_cc65) || !api_pop_int32_end(&ofs))
        return api_return_errno(API_EINVAL);
    if (!fd->lseek)
        return api_return_errno(API_ENOSYS);
    /* cc65 whence: 2=SET, 0=CUR, 1=END -> standard SEEK_*. */
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
    std_fd_t *fd = std_validate_fd(API_A);
    if (!fd)
        return api_return_errno(API_EBADF);
    int8_t whence;
    int32_t ofs;
    if (!api_pop_int8(&whence) || !api_pop_int32_end(&ofs))
        return api_return_errno(API_EINVAL);
    if (!fd->lseek)
        return api_return_errno(API_ENOSYS);
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)
        return api_return_errno(API_EINVAL);
    return std_lseek_common(fd, whence, ofs);
}

bool std_api_syncfs(void)
{
    std_fd_t *fd = std_validate_fd(API_A);
    if (!fd)
        return api_return_errno(API_EBADF);
    if (!fd->sync) /* console streams + read-only drives don't sync */
        return api_return_errno(API_ENOSYS);
    api_errno err = API_EIO;
    std_rw_result r = fd->sync(fd->desc, &err); /* persist MSC0: writes (web: IndexedDB) */
    if (r == STD_ERROR)
        return api_return_errno(err);
    return api_return_ax(0);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* Pump the line editor: drains keyboard + terminal replies, echoes, and fires
 * the read callback when a line completes. Called once per frame. */
void std_task(void)
{
    rln_task();
}

void std_reset(void)
{
    std_files_reset(); /* close open files (driver close frees their objects) */
    dir_stop();        /* close open FatFs directories (ria/api/dir.c) */
    host_dir_stop();   /* close open host directories */
    setup_console();   /* re-establish fd 0-4 */
    rd.active = false;
    wr.active = false;
    std_rln_active = false;
    std_rln_needs_nl = false;
    std_rln_buf = NULL;
    std_rln_pos = 0;
    std_rln_len = 0;
    com_reset();
    rln_init();
}
