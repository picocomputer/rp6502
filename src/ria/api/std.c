/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/api.h"
#include "api/std.h"
#include "sys/com.h"
#include "sys/pix.h"
#include "sys/rln.h"
#include "net/mdm.h"
#include "fatfs/ff.h"
#include <stdio.h>

#if defined(DEBUG_RIA_API) || defined(DEBUG_RIA_API_STD)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define STD_FIL_MAX 8
static FIL std_fil[STD_FIL_MAX];

// File descriptor table with function pointers for polymorphic I/O
#define STD_FD_MAX 16
typedef struct
{
    bool is_open;
    bool (*close)(void);
    bool (*read_xstack)(void);
    bool (*read_xram)(void);
    bool (*write_xstack)(void);
    bool (*write_xram)(void);
    FIL *fatfs;
} std_fd_t;
static std_fd_t std_fd[STD_FD_MAX];

// Reserved file descriptors
#define STD_FD_STDIN 0
#define STD_FD_STDOUT 1
#define STD_FD_STDERR 2
#define STD_FD_FIRST_FREE 3
static_assert(STD_FD_MAX < 128);

// Active operation state - set before calling handlers
static std_fd_t *std_op;
static char *std_buf;
static uint16_t std_len;
static uint16_t std_pos;

// PIX transfer state for xram operations
static int32_t std_xram_pix_remaining = -1;

// Readline state for stdin
static bool std_rln_active;
static const char *std_rln_buf;
static bool std_rln_needs_nl;
static size_t std_rln_pos;
static size_t std_rln_len;
static size_t std_rln_max_len;
static uint32_t std_rln_ctrl_bits;

// ============================================================================
// Readline helpers for stdin
// ============================================================================

static void std_rln_callback(bool timeout, const char *buf, size_t length)
{
    (void)timeout;
    assert(!timeout);
    std_rln_active = false;
    std_rln_buf = buf;
    std_rln_pos = 0;
    std_rln_len = length;
    std_rln_needs_nl = true;
}

static bool std_rln_ready(void)
{
    if (std_rln_needs_nl || std_rln_pos < std_rln_len)
        return true;
    if (!std_rln_active)
    {
        std_rln_active = true;
        rln_read_line(0, std_rln_callback, std_rln_max_len + 1, std_rln_ctrl_bits);
    }
    return false;
}

static size_t std_rln_read(uint8_t *buf, size_t count)
{
    size_t i;
    for (i = 0; i < count && std_rln_pos < std_rln_len; i++)
        buf[i] = std_rln_buf[std_rln_pos++];
    if (i < count && std_rln_needs_nl)
    {
        buf[i++] = '\n';
        std_rln_needs_nl = false;
    }
    return i;
}

// ============================================================================
// xstack finish helper - relocates buffer in xstack
// ============================================================================

static bool std_xstack_finish(void)
{
    uint8_t *buf = (uint8_t *)std_buf;
    uint16_t count = std_len;
    xstack_ptr = XSTACK_SIZE;
    if (std_pos == count)
        xstack_ptr -= count;
    else
        for (uint16_t i = std_pos; i;)
            xstack[--xstack_ptr] = buf[--i];
    std_op = NULL;
    return api_return_ax(std_pos);
}

// ============================================================================
// xram PIX transfer helper
// ============================================================================

static bool std_xram_pix_send(void)
{
    uint16_t xram_addr = std_buf - (char *)xram;
    for (; std_xram_pix_remaining > 0 && pix_ready(); --std_xram_pix_remaining, ++xram_addr, ++std_buf)
        pix_send(PIX_DEVICE_XRAM, 0, xram[xram_addr], xram_addr);
    if (std_xram_pix_remaining <= 0)
    {
        std_xram_pix_remaining = -1;
        std_op = NULL;
        return api_return();
    }
    return api_working();
}

// ============================================================================
// stdin handlers
// ============================================================================

static bool std_stdin_close(void)
{
    return api_return_errno(API_EINVAL);
}

static bool std_stdin_read_xstack(void)
{
    if (!std_rln_ready())
        return api_working();
    std_pos = std_rln_read((uint8_t *)std_buf, std_len);
    return std_xstack_finish();
}

static bool std_stdin_read_xram(void)
{
    // Continue PIX transfer
    if (std_xram_pix_remaining >= 0)
        return std_xram_pix_send();
    // Read data
    if (!std_rln_ready())
        return api_working();
    std_pos = std_rln_read((uint8_t *)std_buf, std_len);
    // Start PIX transfer
    api_set_ax(std_pos);
    std_xram_pix_remaining = std_pos;
    return api_working();
}

static bool std_stdin_write_xstack(void)
{
    return api_return_errno(API_EINVAL);
}

static bool std_stdin_write_xram(void)
{
    return api_return_errno(API_EINVAL);
}

// ============================================================================
// stdout/stderr handlers
// ============================================================================

static bool std_stdout_close(void)
{
    return api_return_errno(API_EINVAL);
}

static bool std_stdout_read_xstack(void)
{
    return api_return_errno(API_EINVAL);
}

static bool std_stdout_read_xram(void)
{
    return api_return_errno(API_EINVAL);
}

static bool std_stdout_write_xstack(void)
{
    while (std_pos < std_len && com_putchar_ready())
        putchar(std_buf[std_pos++]);
    if (std_pos >= std_len)
    {
        uint16_t result = std_pos;
        std_op = NULL;
        return api_return_ax(result);
    }
    return api_working();
}

static bool std_stdout_write_xram(void)
{
    while (std_pos < std_len && com_putchar_ready())
        putchar(std_buf[std_pos++]);
    if (std_pos >= std_len)
    {
        uint16_t result = std_pos;
        std_op = NULL;
        return api_return_ax(result);
    }
    return api_working();
}

// ============================================================================
// modem handlers
// ============================================================================

static bool std_mdm_close(void)
{
    std_op->is_open = false;
    std_op = NULL;
    if (mdm_close())
        return api_return_ax(0);
    return api_return_fresult(FR_INVALID_OBJECT);
}

static bool std_mdm_read_xstack(void)
{
    while (std_pos < std_len)
    {
        switch (mdm_rx(&std_buf[std_pos]))
        {
        case -1:
            std_op = NULL;
            return api_return_fresult(FR_INVALID_OBJECT);
        case 1:
            std_pos++;
            return api_working();
        case 0:
            goto done;
        }
    }
done:
    return std_xstack_finish();
}

static bool std_mdm_read_xram(void)
{
    // Continue PIX transfer
    if (std_xram_pix_remaining >= 0)
        return std_xram_pix_send();
    // Read data
    while (std_pos < std_len)
    {
        switch (mdm_rx(&std_buf[std_pos]))
        {
        case -1:
            std_op = NULL;
            return api_return_fresult(FR_INVALID_OBJECT);
        case 1:
            std_pos++;
            return api_working();
        case 0:
            goto done;
        }
    }
done:
    // Start PIX transfer
    api_set_ax(std_pos);
    std_xram_pix_remaining = std_pos;
    return api_working();
}

static bool std_mdm_write_xstack(void)
{
    while (std_pos < std_len)
    {
        int tx = mdm_tx(std_buf[std_pos]);
        if (tx == -1)
        {
            std_op = NULL;
            return api_return_fresult(FR_INVALID_OBJECT);
        }
        if (tx == 0)
            break;
        std_pos++;
        return api_working();
    }
    uint16_t result = std_pos;
    std_op = NULL;
    return api_return_ax(result);
}

static bool std_mdm_write_xram(void)
{
    while (std_pos < std_len)
    {
        int tx = mdm_tx(std_buf[std_pos]);
        if (tx == -1)
        {
            std_op = NULL;
            return api_return_fresult(FR_INVALID_OBJECT);
        }
        if (tx == 0)
            break;
        std_pos++;
        return api_working();
    }
    uint16_t result = std_pos;
    std_op = NULL;
    return api_return_ax(result);
}

// ============================================================================
// fatfs handlers
// ============================================================================

static bool std_fatfs_close(void)
{
    FIL *fp = std_op->fatfs;
    std_op->is_open = false;
    std_op->fatfs = NULL;
    std_op = NULL;
    FRESULT fresult = f_close(fp);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    return api_return_ax(0);
}

static bool std_fatfs_read_xstack(void)
{
    FIL *fp = std_op->fatfs;
    UINT br;
    FRESULT fresult = f_read(fp, std_buf, std_len, &br);
    if (fresult != FR_OK)
    {
        std_op = NULL;
        return api_return_fresult(fresult);
    }
    std_pos = br;
    return std_xstack_finish();
}

static bool std_fatfs_read_xram(void)
{
    // Continue PIX transfer
    if (std_xram_pix_remaining >= 0)
        return std_xram_pix_send();
    // Read data
    FIL *fp = std_op->fatfs;
    UINT br;
    FRESULT fresult = f_read(fp, std_buf, std_len, &br);
    if (fresult != FR_OK)
    {
        std_op = NULL;
        return api_return_fresult(fresult);
    }
    std_pos = br;
    // Start PIX transfer
    api_set_ax(std_pos);
    std_xram_pix_remaining = std_pos;
    return api_working();
}

static bool std_fatfs_write_xstack(void)
{
    FIL *fp = std_op->fatfs;
    UINT bw;
    FRESULT fresult = f_write(fp, std_buf, std_len, &bw);
    std_op = NULL;
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    return api_return_ax(bw);
}

static bool std_fatfs_write_xram(void)
{
    FIL *fp = std_op->fatfs;
    UINT bw;
    FRESULT fresult = f_write(fp, std_buf, std_len, &bw);
    std_op = NULL;
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    return api_return_ax(bw);
}

// ============================================================================
// Helpers
// ============================================================================

static int std_find_free_fd(void)
{
    for (int fd = STD_FD_FIRST_FREE; fd < STD_FD_MAX; fd++)
        if (!std_fd[fd].is_open)
            return fd;
    return -1;
}

static FIL *std_find_free_fil(void)
{
    for (int i = 0; i < STD_FIL_MAX; i++)
        if (!std_fil[i].obj.fs)
            return &std_fil[i];
    return NULL;
}

static std_fd_t *std_validate_fd(int fd)
{
    if (fd < 0 || fd >= STD_FD_MAX || !std_fd[fd].is_open)
        return NULL;
    return &std_fd[fd];
}

// ============================================================================
// open/close API
// ============================================================================

bool std_api_open(void)
{
    const unsigned char RDWR = 0x03;
    const unsigned char CREAT = 0x10;
    const unsigned char TRUNC = 0x20;
    const unsigned char APPEND = 0x40;
    const unsigned char EXCL = 0x80;

    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;

    int fd = std_find_free_fd();
    if (fd < 0)
        return api_return_errno(API_EMFILE);

    // Check for special device: modem
    if (mdm_open(path))
    {
        std_fd[fd].is_open = true;
        std_fd[fd].close = std_mdm_close;
        std_fd[fd].read_xstack = std_mdm_read_xstack;
        std_fd[fd].read_xram = std_mdm_read_xram;
        std_fd[fd].write_xstack = std_mdm_write_xstack;
        std_fd[fd].write_xram = std_mdm_write_xram;
        std_fd[fd].fatfs = NULL;
        return api_return_ax(fd);
    }

    // FatFs file
    uint8_t flags = API_A;
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
        return api_return_errno(API_EMFILE);

    FRESULT fresult = f_open(fp, path, mode);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);

    std_fd[fd].is_open = true;
    std_fd[fd].close = std_fatfs_close;
    std_fd[fd].read_xstack = std_fatfs_read_xstack;
    std_fd[fd].read_xram = std_fatfs_read_xram;
    std_fd[fd].write_xstack = std_fatfs_write_xstack;
    std_fd[fd].write_xram = std_fatfs_write_xram;
    std_fd[fd].fatfs = fp;
    return api_return_ax(fd);
}

bool std_api_close(void)
{
    int fd = API_A;
    if (fd < STD_FD_FIRST_FREE || fd >= STD_FD_MAX || !std_fd[fd].is_open)
        return api_return_errno(API_EINVAL);
    std_op = &std_fd[fd];
    return std_op->close();
}

// int stdin_opt(unsigned long ctrl_bits, unsigned char str_length)
bool std_api_stdin_opt(void)
{
    uint8_t str_length = API_A;
    uint32_t ctrl_bits;
    if (!api_pop_uint32_end(&ctrl_bits))
        return api_return_errno(API_EINVAL);
    std_rln_max_len = str_length;
    std_rln_ctrl_bits = ctrl_bits;
    return api_return_ax(0);
}

// ============================================================================
// read/write API - pure dispatch
// ============================================================================

bool std_api_read_xstack(void)
{
    if (std_op)
        return std_op->read_xstack();

    uint16_t count;
    int fd = API_A;
    if (!api_pop_uint16_end(&count) || count > XSTACK_SIZE)
        return api_return_errno(API_EINVAL);
    std_op = std_validate_fd(fd);
    if (!std_op)
        return api_return_errno(API_EINVAL);

    std_buf = (char *)&xstack[XSTACK_SIZE - count];
    std_len = count;
    std_pos = 0;
    return std_op->read_xstack();
}

bool std_api_read_xram(void)
{
    if (std_op)
        return std_op->read_xram();

    uint16_t count, xram_addr;
    int fd = API_A;
    if (!api_pop_uint16(&count) || !api_pop_uint16_end(&xram_addr))
        return api_return_errno(API_EINVAL);
    std_op = std_validate_fd(fd);
    if (!std_op)
        return api_return_errno(API_EINVAL);
    if (count > 0x7FFF)
        count = 0x7FFF;
    std_buf = (char *)&xram[xram_addr];
    if (std_buf + count > (char *)xram + 0x10000)
        return api_return_errno(API_EINVAL);

    std_len = count;
    std_pos = 0;
    return std_op->read_xram();
}

bool std_api_write_xstack(void)
{
    if (std_op)
        return std_op->write_xstack();

    int fd = API_A;
    std_op = std_validate_fd(fd);
    if (!std_op)
        return api_return_errno(API_EINVAL);

    std_len = XSTACK_SIZE - xstack_ptr;
    std_buf = (char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    std_pos = 0;
    return std_op->write_xstack();
}

bool std_api_write_xram(void)
{
    if (std_op)
        return std_op->write_xram();

    uint16_t xram_addr, count;
    int fd = API_A;
    if (!api_pop_uint16(&count) || !api_pop_uint16_end(&xram_addr))
        return api_return_errno(API_EINVAL);
    std_op = std_validate_fd(fd);
    if (!std_op)
        return api_return_errno(API_EINVAL);
    if (count > 0x7FFF)
        count = 0x7FFF;
    std_buf = (char *)&xram[xram_addr];
    if (std_buf + count > (char *)xram + 0x10000)
        return api_return_errno(API_EINVAL);

    std_len = count;
    std_pos = 0;
    return std_op->write_xram();
}

// ============================================================================
// lseek and syncfs - FatFs only
// ============================================================================

static bool std_api_lseek(int set, int cur, int end)
{
    int8_t whence;
    int32_t ofs;
    int fd = API_A;
    if (fd < STD_FD_FIRST_FREE || fd >= STD_FD_MAX ||
        !std_fd[fd].is_open || !std_fd[fd].fatfs ||
        !api_pop_int8(&whence) || !api_pop_int32_end(&ofs))
        return api_return_errno(API_EINVAL);
    FIL *fp = std_fd[fd].fatfs;
    if (whence == set)
        ;
    else if (whence == cur)
        ofs += f_tell(fp);
    else if (whence == end)
        ofs += f_size(fp);
    else
        return api_return_errno(API_EINVAL);
    FRESULT fresult = f_lseek(fp, ofs);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    FSIZE_t pos = f_tell(fp);
    if (pos > 0x7FFFFFFF)
        pos = 0x7FFFFFFF;
    return api_return_axsreg(pos);
}

bool std_api_lseek_cc65(void)
{
    return std_api_lseek(2, 0, 1);
}

bool std_api_lseek_llvm(void)
{
    return std_api_lseek(0, 1, 2);
}

bool std_api_syncfs(void)
{
    int fd = API_A;
    if (fd < STD_FD_FIRST_FREE || fd >= STD_FD_MAX ||
        !std_fd[fd].is_open || !std_fd[fd].fatfs)
        return api_return_errno(API_EINVAL);
    FRESULT fresult = f_sync(std_fd[fd].fatfs);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    return api_return_ax(0);
}

// ============================================================================
// Initialization and cleanup
// ============================================================================

void std_run(void)
{
    std_op = NULL;
    std_xram_pix_remaining = -1;
    std_rln_active = false;
    std_rln_needs_nl = false;
    std_rln_pos = 0;
    std_rln_len = 0;
    std_rln_max_len = 254;
    std_rln_ctrl_bits = 0;

    for (int i = 0; i < STD_FD_MAX; i++)
    {
        std_fd[i].is_open = false;
        std_fd[i].close = NULL;
        std_fd[i].read_xstack = NULL;
        std_fd[i].read_xram = NULL;
        std_fd[i].write_xstack = NULL;
        std_fd[i].write_xram = NULL;
        std_fd[i].fatfs = NULL;
    }

    std_fd[STD_FD_STDIN].is_open = true;
    std_fd[STD_FD_STDIN].close = std_stdin_close;
    std_fd[STD_FD_STDIN].read_xstack = std_stdin_read_xstack;
    std_fd[STD_FD_STDIN].read_xram = std_stdin_read_xram;
    std_fd[STD_FD_STDIN].write_xstack = std_stdin_write_xstack;
    std_fd[STD_FD_STDIN].write_xram = std_stdin_write_xram;

    std_fd[STD_FD_STDOUT].is_open = true;
    std_fd[STD_FD_STDOUT].close = std_stdout_close;
    std_fd[STD_FD_STDOUT].read_xstack = std_stdout_read_xstack;
    std_fd[STD_FD_STDOUT].read_xram = std_stdout_read_xram;
    std_fd[STD_FD_STDOUT].write_xstack = std_stdout_write_xstack;
    std_fd[STD_FD_STDOUT].write_xram = std_stdout_write_xram;

    std_fd[STD_FD_STDERR].is_open = true;
    std_fd[STD_FD_STDERR].close = std_stdout_close;
    std_fd[STD_FD_STDERR].read_xstack = std_stdout_read_xstack;
    std_fd[STD_FD_STDERR].read_xram = std_stdout_read_xram;
    std_fd[STD_FD_STDERR].write_xstack = std_stdout_write_xstack;
    std_fd[STD_FD_STDERR].write_xram = std_stdout_write_xram;
}

void std_stop(void)
{
    for (int i = 0; i < STD_FIL_MAX; i++)
        if (std_fil[i].obj.fs)
            f_close(&std_fil[i]);
}
