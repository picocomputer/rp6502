/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The 6502 stdio/file syscalls (ria/api/std.c ops 0x14-0x1E). This is the stdio
 * OPEN DISPATCHER: a path is claimed by the first driver whose handles() returns
 * true — ROM: first, then the writable-filesystem catch-all (g_fs_driver, the host
 * or a mounted RAM FatFs), exactly as the firmware's ria/api/std.c matches. The
 * drivers share the firmware's std_driver_t ABI (int descriptor, std_rw_result,
 * api_errno); open() caches the driver's ops in the fd pool so later ops dispatch
 * without re-parsing the path.
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
#include "api/api.h"
#include "api/std.h"
#include "aud/bel.h"
#include "str/rln.h"
#include "sys/com.h"
#include <stdio.h> /* SEEK_SET/SEEK_CUR/SEEK_END */
#include <string.h>
#include <strings.h> /* strcasecmp */

#define FD_STDIN 0
#define FD_STDOUT 1
#define FD_STDERR 2
#define FD_CON 3
#define FD_TTY 4
#define FD_FIRST_FREE 5
#define FD_MAX 16

/* The active writable-filesystem driver (std.c's catch-all): the native host by
 * default, a RAM FatFs while --tmpdrive is mounted. Swapped at runtime by the
 * drive lifecycle (emu/usb/msc.c). ROM: is always tried first (below). */
static const std_driver_t *g_fs_driver = &host_file_driver;

void emu_set_fs_driver(const std_driver_t *drv)
{
    g_fs_driver = drv;
}

/* The per-fd operations cached from open(). The console streams set these
 * directly; open()ed files copy them from the matching driver. */
typedef struct
{
    bool is_open;
    int desc;
    std_rw_result (*close)(int, api_errno *);
    std_rw_result (*read)(int, char *, uint32_t, uint32_t *, api_errno *);
    std_rw_result (*write)(int, const char *, uint32_t, uint32_t *, api_errno *);
    std_rw_result (*sync)(int, api_errno *);
    int (*lseek)(int, int8_t, int32_t, int32_t *, api_errno *);
} std_fd_t;
static std_fd_t fds[FD_MAX];

/* ------------------------------------------------------------------ */
/* Console streams: stdin via the line editor, writes to the terminal  */
/* ------------------------------------------------------------------ */

static bool rln_busy;
static const char *rln_line;
static bool rln_needs_nl;
static size_t rln_line_pos;
static size_t rln_line_len;

static void stdin_callback(bool timeout, const char *buf)
{
    (void)timeout;
    rln_busy = false;
    rln_line = buf;
    rln_line_pos = 0;
    rln_line_len = strlen(buf);
    rln_needs_nl = true;
}

/* stdin/con read (ports the firmware std_stdin_read): STD_PENDING with no line
 * ready (a request is queued; the machine re-dispatches us), STD_OK once bytes
 * (and the trailing newline) are delivered. */
static std_rw_result con_read(int desc, char *buf, uint32_t count, uint32_t *got, api_errno *err)
{
    (void)desc, (void)err;
    *got = 0;
    if (!rln_needs_nl && rln_line_pos >= rln_line_len)
    {
        if (!rln_busy)
        {
            rln_busy = true;
            rln_read_line(stdin_callback);
        }
        return STD_PENDING;
    }
    uint32_t i = 0;
    for (; i < count && rln_line_pos < rln_line_len; i++)
        buf[i] = rln_line[rln_line_pos++];
    if (i < count && rln_needs_nl)
    {
        buf[i++] = '\n';
        rln_needs_nl = false;
    }
    *got = i;
    return STD_OK;
}

/* CON: read (ports the firmware std_con_read): non-blocking — no line ready reads
 * 0 bytes rather than spinning the 6502 (unlike stdin). */
static std_rw_result con_read_nonblock(int desc, char *buf, uint32_t count, uint32_t *got, api_errno *err)
{
    std_rw_result r = con_read(desc, buf, count, got, err);
    return (r == STD_PENDING) ? STD_OK : r;
}

/* TTY: read (ports the firmware std_tty_read): raw, non-blocking drain of queued
 * keystroke bytes — no cooking/echo, *got=0 when idle. */
static std_rw_result tty_read(int desc, char *buf, uint32_t count, uint32_t *got, api_errno *err)
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

/* stdout/stderr/con/tty write: to the terminal, instantly (no drain). */
static std_rw_result con_write(int desc, const char *buf, uint32_t count, uint32_t *put, api_errno *err)
{
    (void)desc, (void)err;
    std_bell_scan(buf, count);
    emu_stdout_write(buf, count);
    *put = count;
    return STD_OK;
}

static void setup_console(void)
{
    memset(fds, 0, sizeof(fds));
    fds[FD_STDIN] = (std_fd_t){.is_open = true, .read = con_read};
    fds[FD_STDOUT] = (std_fd_t){.is_open = true, .write = con_write};
    fds[FD_STDERR] = (std_fd_t){.is_open = true, .write = con_write};
    fds[FD_CON] = (std_fd_t){.is_open = true, .read = con_read_nonblock, .write = con_write};
    fds[FD_TTY] = (std_fd_t){.is_open = true, .read = tty_read, .write = con_write};
}

/* ------------------------------------------------------------------ */
/* Host-side file API over the driver table (the 6502 reaches it via    */
/* the std_api_* syscalls below; main.c/tests use it directly)          */
/* ------------------------------------------------------------------ */

int std_open(const char *path, uint8_t flags, api_errno *err)
{
    if (strcasecmp(path, "CON:") == 0)
        return FD_CON;
    if (strcasecmp(path, "TTY:") == 0)
        return FD_TTY;
    int fd = -1;
    for (int i = FD_FIRST_FREE; i < FD_MAX; i++)
        if (!fds[i].is_open)
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
    /* ROM: first, then the writable-filesystem catch-all — the firmware order. */
    const std_driver_t *drivers[2] = {&rom_file_driver, g_fs_driver};
    for (int i = 0; i < 2; i++)
    {
        if (!drivers[i]->handles(path))
            continue;
        api_errno e = API_EIO;
        int desc = drivers[i]->open(path, flags, &e);
        if (desc < 0)
        {
            if (err)
                *err = e;
            return -1;
        }
        fds[fd].is_open = true;
        fds[fd].desc = desc;
        fds[fd].close = drivers[i]->close;
        fds[fd].read = drivers[i]->read;
        fds[fd].write = drivers[i]->write;
        fds[fd].sync = drivers[i]->sync;
        fds[fd].lseek = drivers[i]->lseek;
        return fd;
    }
    if (err)
        *err = API_ENOENT; /* unreachable: the host driver is a catch-all */
    return -1;
}

bool std_writable(int fd)
{
    return fd >= 0 && fd < FD_MAX && fds[fd].is_open && fds[fd].write != NULL;
}

std_rw_result std_read(int fd, char *buf, uint32_t n, uint32_t *got, api_errno *err)
{
    *got = 0;
    if (fd < 0 || fd >= FD_MAX || !fds[fd].is_open || !fds[fd].read)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    return fds[fd].read(fds[fd].desc, buf, n, got, err);
}

std_rw_result std_write(int fd, const char *buf, uint32_t n, uint32_t *put, api_errno *err)
{
    *put = 0;
    if (fd < 0 || fd >= FD_MAX || !fds[fd].is_open || !fds[fd].write)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    return fds[fd].write(fds[fd].desc, buf, n, put, err);
}

long std_lseek(int fd, long offset, int whence)
{
    if (fd < 0 || fd >= FD_MAX || !fds[fd].is_open || !fds[fd].lseek)
        return -1;
    int32_t pos;
    api_errno err;
    if (fds[fd].lseek(fds[fd].desc, (int8_t)whence, (int32_t)offset, &pos, &err) < 0)
        return -1;
    return pos;
}

void std_close(int fd)
{
    if (fd < FD_FIRST_FREE || fd >= FD_MAX || !fds[fd].is_open)
        return; /* console streams (0-4) stay open */
    if (fds[fd].close)
    {
        api_errno err;
        fds[fd].close(fds[fd].desc, &err);
    }
    memset(&fds[fd], 0, sizeof(fds[fd]));
}

/* Close every open file fd (the driver close frees its descriptor); machine
 * reset. The console streams are re-established by setup_console (std_reset). */
void std_files_reset(void)
{
    for (int fd = FD_FIRST_FREE; fd < FD_MAX; fd++)
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
    if (fd == FD_CON || fd == FD_TTY)
        return api_return_ax(0); /* CON:/TTY: stay open; 0/1/2 -> EBADF */
    if (fd < FD_FIRST_FREE || fd >= FD_MAX || !fds[fd].is_open)
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
        int fd = API_A;
        if (fd < 0 || fd >= FD_MAX || !fds[fd].is_open)
            return api_return_errno(API_EBADF);
        if (!fds[fd].read)
            return api_return_errno(API_ENOSYS);
        rd.active = true;
        rd.xram = false;
        rd.fd = fd;
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
        int fd = API_A;
        if (fd < 0 || fd >= FD_MAX || !fds[fd].is_open)
            return api_return_errno(API_EBADF);
        if (!fds[fd].read)
            return api_return_errno(API_ENOSYS);
        if (size > 0x7FFF)
            size = 0x7FFF;
        if ((int)xram_addr + size > 0x10000)
            size = (uint16_t)(0x10000 - xram_addr);
        rd.active = true;
        rd.xram = true;
        rd.fd = fd;
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
    std_rw_result r = fds[wr.fd].write(fds[wr.fd].desc, wr.buf, wr.size, &put, &wr.err);
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
        int fd = API_A;
        uint16_t size = (uint16_t)(XSTACK_SIZE - xstack_ptr);
        const char *buf = (const char *)&xstack[xstack_ptr];
        xstack_ptr = XSTACK_SIZE;
        if (fd < 0 || fd >= FD_MAX || !fds[fd].is_open)
            return api_return_errno(API_EBADF);
        if (!fds[fd].write)
            return api_return_errno(API_ENOSYS);
        wr.active = true;
        wr.fd = fd;
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
        int fd = API_A;
        if (size > 0x7FFF)
            size = 0x7FFF;
        if ((int)xram_addr + size > 0x10000)
            return api_return_errno(API_EINVAL);
        if (fd < 0 || fd >= FD_MAX || !fds[fd].is_open)
            return api_return_errno(API_EBADF);
        if (!fds[fd].write)
            return api_return_errno(API_ENOSYS);
        wr.active = true;
        wr.fd = fd;
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

static bool std_lseek_dispatch(int fd, int8_t whence, int32_t ofs)
{
    int32_t pos;
    api_errno err = API_EIO;
    if (fds[fd].lseek(fds[fd].desc, whence, ofs, &pos, &err) < 0)
        return api_return_errno(err);
    return api_return_axsreg((uint32_t)pos);
}

bool std_api_lseek_cc65(void)
{
    int fd = API_A;
    if (fd < 0 || fd >= FD_MAX || !fds[fd].is_open)
        return api_return_errno(API_EBADF);
    int8_t whence_cc65;
    int32_t ofs;
    if (!api_pop_int8(&whence_cc65) || !api_pop_int32_end(&ofs))
        return api_return_errno(API_EINVAL);
    if (!fds[fd].lseek)
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
    return std_lseek_dispatch(fd, whence, ofs);
}

bool std_api_lseek_llvm(void)
{
    int fd = API_A;
    if (fd < 0 || fd >= FD_MAX || !fds[fd].is_open)
        return api_return_errno(API_EBADF);
    int8_t whence;
    int32_t ofs;
    if (!api_pop_int8(&whence) || !api_pop_int32_end(&ofs))
        return api_return_errno(API_EINVAL);
    if (!fds[fd].lseek)
        return api_return_errno(API_ENOSYS);
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)
        return api_return_errno(API_EINVAL);
    return std_lseek_dispatch(fd, whence, ofs);
}

bool std_api_syncfs(void)
{
    int fd = API_A;
    if (fd < 0 || fd >= FD_MAX || !fds[fd].is_open)
        return api_return_errno(API_EBADF);
    if (!fds[fd].sync) /* console streams + read-only drives don't sync */
        return api_return_errno(API_ENOSYS);
    api_errno err = API_EIO;
    std_rw_result r = fds[fd].sync(fds[fd].desc, &err); /* persist MSC0: writes (web: IndexedDB) */
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
    emu_dir_stop();    /* close open directories on the active backend */
    setup_console();   /* re-establish fd 0-4 */
    rd.active = false;
    wr.active = false;
    rln_busy = false;
    rln_needs_nl = false;
    rln_line = NULL;
    rln_line_pos = 0;
    rln_line_len = 0;
    com_reset();
    rln_init();
}
