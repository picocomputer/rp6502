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

#define STD_FIL_MAX 16
FIL std_fil[STD_FIL_MAX];
#define STD_FIL_STDIN 0
#define STD_FIL_STDOUT 1
#define STD_FIL_STDERR 2
#define STD_FIL_MODEM 3
#define STD_FIL_OFFS 4
static_assert(STD_FIL_MAX + STD_FIL_OFFS < 128);

static int32_t std_xram_count = -1;
static int32_t std_cpu_count = -1;
static int32_t std_mdm_count = -1;
static int32_t std_bytes_moved;
static char *std_buf_ptr;

// TODO simplify this once we drop const buf
static bool std_stdin_active;
static const char *std_stdin_buf;
static bool std_stdin_needs_nl;
static size_t std_stdin_pos;
static size_t std_stdin_length;
static size_t std_api_str_length;
static uint32_t std_api_ctrl_bits;

static void std_stdin_callback(bool timeout, char *buf, size_t length)
{
    (void)timeout;
    assert(!timeout);
    std_stdin_active = false;
    std_stdin_buf = buf;
    std_stdin_pos = 0;
    std_stdin_length = length;
    std_stdin_needs_nl = true;
}

static void std_stdin_request(void)
{
    if (!std_stdin_needs_nl)
    {
        std_stdin_active = true;
        rln_read_line(0, std_stdin_callback, std_api_str_length + 1, std_api_ctrl_bits);
    }
}

static bool std_stdin_ready(void)
{
    return !std_stdin_active;
}

static size_t std_stdin_read(uint8_t *buf, size_t count)
{
    size_t i;
    for (i = 0; i < count && std_stdin_pos < std_stdin_length; i++)
        buf[i] = std_stdin_buf[std_stdin_pos++];
    if (i < count && std_stdin_needs_nl)
    {
        buf[i++] = '\n';
        std_stdin_needs_nl = false;
    }
    return i;
}

bool std_api_stdin_opt(void)
{
    uint8_t str_length = API_A;
    uint32_t ctrl_bits;
    if (!api_pop_uint32_end(&ctrl_bits))
        return api_return_errno(API_EINVAL);
    std_api_str_length = str_length;
    std_api_ctrl_bits = ctrl_bits;
    return api_return_ax(0);
}

bool std_api_open(void)
{
    // These match CC65 which is closer to POSIX than FatFs.
    const unsigned char RDWR = 0x03;
    const unsigned char CREAT = 0x10;
    const unsigned char TRUNC = 0x20;
    const unsigned char APPEND = 0x40;
    const unsigned char EXCL = 0x80;

    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    api_zxstack();
    if (mdm_open(path))
        return api_return_ax(STD_FIL_MODEM);
    uint8_t flags = API_A;
    uint8_t mode = flags & RDWR; // RDWR are same bits
    if (flags & CREAT)
    {
        if (flags & EXCL)
            mode |= FA_CREATE_NEW;
        else
        {
            if (flags & TRUNC)
                mode |= FA_CREATE_ALWAYS;
            else if (flags & APPEND)
                mode |= FA_OPEN_APPEND;
            else
                mode |= FA_OPEN_ALWAYS;
        }
    }
    int fd = 0;
    for (; fd < STD_FIL_MAX; fd++)
        if (!std_fil[fd].obj.fs)
            break;
    if (fd == STD_FIL_MAX)
        return api_return_errno(API_EMFILE);
    FIL *fp = &std_fil[fd];
    FRESULT fresult = f_open(fp, path, mode);
    if (fresult != FR_OK)
        return api_return_errno(API_EFATFS(fresult));
    return api_return_ax(fd + STD_FIL_OFFS);
}

bool std_api_close(void)
{
    int fd = API_A;
    if (fd == STD_FIL_MODEM)
    {
        if (mdm_close())
            return api_return_ax(0);
        else
            return api_return_errno(API_EFATFS(FR_INVALID_OBJECT));
    }
    if (fd < STD_FIL_OFFS || fd >= STD_FIL_MAX + STD_FIL_OFFS)
        return api_return_errno(API_EINVAL);
    FIL *fp = &std_fil[fd - STD_FIL_OFFS];
    FRESULT fresult = f_close(fp);
    if (fresult != FR_OK)
        return api_return_errno(API_EFATFS(fresult));
    return api_return_ax(0);
}

bool std_api_read_xstack(void)
{
    uint8_t *buf;
    uint16_t count;
    if (std_cpu_count >= 0)
    {
        if (!std_stdin_ready())
            return api_working();
        count = std_cpu_count;
        buf = &xstack[XSTACK_SIZE - count];
        std_bytes_moved = std_stdin_read(buf, count);
        std_cpu_count = -1;
    }
    else if (std_mdm_count >= 0)
    {
        count = std_mdm_count;
        buf = &xstack[XSTACK_SIZE - count];
        if (std_bytes_moved < count)
            switch (mdm_rx((char *)&buf[std_bytes_moved]))
            {
            case -1:
                std_mdm_count = -1;
                return api_return_errno(API_EFATFS(FR_INVALID_OBJECT));
            case 1:
                std_bytes_moved++;
                return api_working();
            case 0:
                break;
            }
        std_mdm_count = -1;
    }
    else
    {
        int16_t fd = API_A;
        if (!api_pop_uint16_end(&count) ||
            (fd && fd < STD_FIL_MODEM) ||
            fd >= STD_FIL_MAX + STD_FIL_OFFS ||
            count > XSTACK_SIZE)
            return api_return_errno(API_EINVAL);
        buf = &xstack[XSTACK_SIZE - count];
        if (fd == STD_FIL_STDIN)
        {
            std_cpu_count = count;
            std_stdin_request();
            return api_working();
        }
        if (fd == STD_FIL_MODEM)
        {
            std_mdm_count = count;
            std_bytes_moved = 0;
            return api_working();
        }
        FIL *fp = &std_fil[fd - STD_FIL_OFFS];
        UINT br;
        FRESULT fresult = f_read(fp, buf, count, &br);
        std_bytes_moved = br;
        if (fresult != FR_OK)
            return api_return_errno(API_EFATFS(fresult));
    }
    xstack_ptr = XSTACK_SIZE;
    if (std_bytes_moved == count)
        xstack_ptr -= count;
    else // relocate short read
        for (UINT i = std_bytes_moved; i;)
            xstack[--xstack_ptr] = buf[--i];
    api_sync_xstack();
    return api_return_ax(std_bytes_moved);
}

bool std_api_read_xram(void)
{
    if (std_cpu_count >= 0)
    {
        if (!std_stdin_ready())
            return api_working();
        std_xram_count = std_stdin_read((uint8_t *)std_buf_ptr, std_cpu_count);
        api_set_ax(std_xram_count);
        std_cpu_count = -1;
        return api_working();
    }
    if (std_mdm_count >= 0)
    {
        if (std_bytes_moved < std_mdm_count)
            switch (mdm_rx(&std_buf_ptr[std_bytes_moved]))
            {
            case -1:
                std_mdm_count = -1;
                return api_return_errno(API_EFATFS(FR_INVALID_OBJECT));
            case 1:
                std_bytes_moved++;
                return api_working();
            case 0:
                break;
            }
        std_xram_count = std_bytes_moved;
        api_set_ax(std_xram_count);
        std_mdm_count = -1;
        return api_working();
    }
    if (std_xram_count >= 0)
    {
        uint16_t xram_addr = std_buf_ptr - (char *)xram;
        for (; std_xram_count && pix_ready(); --std_xram_count, ++xram_addr, ++std_buf_ptr)
            pix_send(PIX_DEVICE_XRAM, 0, xram[xram_addr], xram_addr);
        if (!std_xram_count)
        {
            std_xram_count = -1;
            api_return_released();
        }
        return api_working();
    }
    uint16_t count;
    uint16_t xram_addr;
    int16_t fd = API_A;
    if (!api_pop_uint16(&count) ||
        !api_pop_uint16_end(&xram_addr) ||
        (fd && fd < STD_FIL_MODEM) ||
        fd >= STD_FIL_MAX + STD_FIL_OFFS)
        return api_return_errno(API_EINVAL);
    std_buf_ptr = (char *)&xram[xram_addr];
    if (fd == STD_FIL_STDIN)
    {
        std_stdin_request();
        std_cpu_count = count;
        return api_working();
    }
    if (fd == STD_FIL_MODEM)
    {
        std_bytes_moved = 0;
        std_mdm_count = count;
        return api_working();
    }
    if (count > 0x7FFF)
        count = 0x7FFF;
    if (std_buf_ptr + count > (char *)xram + 0x10000)
        return api_return_errno(API_EINVAL);
    FIL *fp = &std_fil[fd - STD_FIL_OFFS];
    UINT br;
    FRESULT fresult = f_read(fp, std_buf_ptr, count, &br);
    if (fresult == FR_OK)
        api_set_ax(br);
    else
    {
        API_ERRNO = fresult;
        api_set_ax(-1);
    }
    std_xram_count = br;
    return api_working();
}

static bool std_out_write(void)
{
    if (std_bytes_moved < std_cpu_count && com_tx_printable())
        putchar(std_buf_ptr[std_bytes_moved++]);
    if (std_bytes_moved >= std_cpu_count)
    {
        std_cpu_count = -1;
        return api_return_ax(std_bytes_moved);
    }
    return api_working();
}

static bool std_mdm_write(void)
{
    while (std_bytes_moved < std_mdm_count)
    {
        int tx = mdm_tx(std_buf_ptr[std_bytes_moved]);
        if (tx == -1)
        {
            std_mdm_count = -1;
            return api_return_errno(API_EFATFS(FR_INVALID_OBJECT));
        }
        if (tx == 0)
            break;
        std_bytes_moved++;
        return api_working();
    }
    std_mdm_count = -1;
    return api_return_ax(std_bytes_moved);
}

bool std_api_write_xstack(void)
{
    if (std_cpu_count >= 0)
        return std_out_write();
    if (std_mdm_count >= 0)
        return std_mdm_write();
    uint16_t count;
    int fd = API_A;
    if (fd == STD_FIL_STDIN || fd >= STD_FIL_MAX + STD_FIL_OFFS)
        return api_return_errno(API_EINVAL);
    count = XSTACK_SIZE - xstack_ptr;
    std_bytes_moved = 0;
    std_buf_ptr = (char *)&xstack[xstack_ptr];
    api_zxstack();
    if (fd == STD_FIL_MODEM)
    {
        std_mdm_count = count;
        return api_working();
    }
    if (fd < STD_FIL_OFFS) // stdout stderr
    {
        std_cpu_count = count;
        return api_working();
    }
    FIL *fp = &std_fil[fd - STD_FIL_OFFS];
    UINT bw;
    FRESULT fresult = f_write(fp, std_buf_ptr, count, &bw);
    if (fresult != FR_OK)
        return api_return_errno(API_EFATFS(fresult));
    return api_return_ax(bw);
}

bool std_api_write_xram(void)
{
    if (std_cpu_count >= 0)
        return std_out_write();
    if (std_mdm_count >= 0)
        return std_mdm_write();
    uint16_t xram_addr;
    uint16_t count;
    int fd = API_A;
    if (fd == STD_FIL_STDIN || fd >= STD_FIL_MAX + STD_FIL_OFFS)
        return api_return_errno(API_EINVAL);
    if (!api_pop_uint16(&count) ||
        !api_pop_uint16_end(&xram_addr))
        return api_return_errno(API_EINVAL);
    std_bytes_moved = 0;
    std_buf_ptr = (char *)&xram[xram_addr];
    if (std_buf_ptr + count > (char *)xram + 0x10000)
        return api_return_errno(API_EINVAL);
    if (count > 0x7FFF)
        count = 0x7FFF;
    if (fd == STD_FIL_MODEM)
    {
        std_mdm_count = count;
        return api_working();
    }
    if (fd < STD_FIL_OFFS) // stdout stderr
    {
        std_cpu_count = count;
        return api_working();
    }
    FIL *fp = &std_fil[fd - STD_FIL_OFFS];
    UINT bw;
    FRESULT fresult = f_write(fp, std_buf_ptr, count, &bw);
    if (fresult != FR_OK)
        return api_return_errno(API_EFATFS(fresult));
    return api_return_ax(bw);
}

bool std_api_lseek(void)
{
    int8_t whence;
    int32_t ofs;
    int fd = API_A;
    if (fd < STD_FIL_OFFS ||
        fd >= STD_FIL_MAX + STD_FIL_OFFS ||
        !api_pop_int8(&whence) ||
        !api_pop_int32_end(&ofs))
        return api_return_errno(API_EINVAL);
    FIL *fp = &std_fil[fd - STD_FIL_OFFS];
    switch (whence) // CC65
    {
    case 0: // SEEK_CUR
        ofs += f_tell(fp);
        break;
    case 1: // SEEK_END
        ofs += f_size(fp);
        break;
    case 2: // SEEK_SET
        break;
    default:
        return api_return_errno(API_EINVAL);
    }
    FRESULT fresult = f_lseek(fp, ofs);
    if (fresult != FR_OK)
        return api_return_errno(API_EFATFS(fresult));
    FSIZE_t pos = f_tell(fp);
    // Beyond 2GB is darkness.
    if (pos > 0x7FFFFFFF)
        pos = 0x7FFFFFFF;
    return api_return_axsreg(pos);
}

bool std_api_unlink(void)
{
    uint8_t *path = &xstack[xstack_ptr];
    api_zxstack();
    FRESULT fresult = f_unlink((TCHAR *)path);
    if (fresult != FR_OK)
        return api_return_errno(API_EFATFS(fresult));
    return api_return_ax(0);
}

bool std_api_rename(void)
{
    uint8_t *oldname, *newname;
    oldname = newname = &xstack[xstack_ptr];
    api_zxstack();
    while (*oldname)
        oldname++;
    if (oldname == &xstack[XSTACK_SIZE])
        return api_return_errno(API_EINVAL);
    oldname++;
    FRESULT fresult = f_rename((TCHAR *)oldname, (TCHAR *)newname);
    if (fresult != FR_OK)
        return api_return_errno(API_EFATFS(fresult));
    return api_return_ax(0);
}

void std_run(void)
{
    std_stdin_active = false;
    std_stdin_needs_nl = false;
    std_stdin_pos = 0;
    std_stdin_length = 0;
    std_api_str_length = 254;
    std_api_ctrl_bits = 0;
}

void std_stop(void)
{
    std_xram_count = -1;
    std_cpu_count = -1;
    std_mdm_count = -1;
    for (int i = 0; i < STD_FIL_MAX; i++)
        if (std_fil[i].obj.fs)
            f_close(&std_fil[i]);
}
